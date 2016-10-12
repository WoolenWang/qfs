//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2015/04/27
// Author: Mike Ovsiannikov
//
// Copyright 2015 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// Transaction log replication transmitter.
//
//
//----------------------------------------------------------------------------

#include "LogTransmitter.h"

#include "MetaRequest.h"
#include "MetaVrOps.h"
#include "MetaVrLogSeq.h"
#include "MetaVrSM.h"
#include "util.h"

#include "common/kfstypes.h"
#include "common/MsgLogger.h"
#include "common/Properties.h"

#include "kfsio/KfsCallbackObj.h"
#include "kfsio/NetConnection.h"
#include "kfsio/NetManager.h"
#include "kfsio/IOBuffer.h"
#include "kfsio/ClientAuthContext.h"
#include "kfsio/event.h"
#include "kfsio/checksum.h"

#include "qcdio/QCUtils.h"
#include "qcdio/qcdebug.h"

#include <string.h>

#include <limits>
#include <string>
#include <algorithm>
#include <set>
#include <deque>
#include <utility>

namespace KFS
{
using std::string;
using std::max;
using std::multiset;
using std::deque;
using std::pair;
using std::find;

class LogTransmitter::Impl
{
private:
    class Transmitter;
    enum { kHeartbeatsPerTimeoutInterval = 2 };
public:
    typedef MetaVrSM::Config      Config;
    typedef Config::NodeId        NodeId;
    typedef QCDLList<Transmitter> List;
    typedef uint32_t              Checksum;

    Impl(
        LogTransmitter& inTransmitter,
        NetManager&     inNetManager,
        CommitObserver& inCommitObserver)
        : mTransmitter(inTransmitter),
          mNetManager(inNetManager),
          mRetryInterval(2),
          mMaxReadAhead(MAX_RPC_HEADER_LEN),
          mHeartbeatInterval(16),
          mMinAckToCommit(numeric_limits<int>::max()),
          mMaxPending(4 << 20),
          mCompactionInterval(256),
          mCommitted(),
          mAuthType(
            kAuthenticationTypeKrb5 |
            kAuthenticationTypeX509 |
            kAuthenticationTypePSK),
          mAuthTypeStr("Krb5 X509 PSK"),
          mCommitObserver(inCommitObserver),
          mIdsCount(0),
          mNodeId(-1),
          mSendingFlag(false),
          mPendingUpdateFlag(false),
          mUpFlag(false),
          mSuspendedFlag(false),
          mFileSystemId(-1),
          mMetaVrSMPtr(0),
          mTransmitterAuthParamsPrefix(),
          mTransmitterAuthParams()
    {
        List::Init(mTransmittersPtr);
        mTmpBuf[kTmpBufSize] = 0;
        mSeqBuf[kSeqBufSize] = 0;
    }
    ~Impl()
        { Impl::Shutdown(); }
    int SetParameters(
        const char*       inParamPrefixPtr,
        const Properties& inParameters);
    int TransmitBlock(
        const MetaVrLogSeq& inBlockSeq,
        int                 inBlockSeqLen,
        const char*         inBlockPtr,
        size_t              inBlockLen,
        Checksum            inChecksum,
        size_t              inChecksumStartPos);
    static seq_t RandomSeq()
    {
        seq_t theReq = 0;
        CryptoKeys::PseudoRand(&theReq, sizeof(theReq));
        return ((theReq < 0 ? -theReq : theReq) >> 1);
    }
    void SetFileSystemId(
        int64_t inFsId)
        { mFileSystemId = inFsId; }
    char* GetParseBufferPtr()
        { return mParseBuffer; }
    NetManager& GetNetManager()
        { return mNetManager; }
    int GetRetryInterval() const
        { return mRetryInterval; }
    int GetMaxReadAhead() const
        { return mMaxReadAhead; }
    int GetHeartbeatInterval() const
        { return mHeartbeatInterval; }
    void SetHeartbeatInterval(
        int inPrimaryTimeoutSec)
    {
        mHeartbeatInterval = max(1, inPrimaryTimeoutSec /
            kHeartbeatsPerTimeoutInterval);
    }
    MetaVrLogSeq GetCommitted() const
        { return mCommitted; }
    int GetMaxPending() const
        { return mMaxPending; }
    int GetCompactionInterval() const
        { return mCompactionInterval; }
    int GetChannelsCount() const
        { return mTransmittersCount; }
    void Shutdown();
    void Acked(
        const MetaVrLogSeq& inPrevAck,
        NodeId              inPrevPrimaryNodeId,
        Transmitter&        inTransmitter);
    void WriteBlock(
        IOBuffer&           inBuffer,
        const MetaVrLogSeq& inBlockSeq,
        int                 inBlockSeqLen,
        const char*         inBlockPtr,
        size_t              inBlockLen,
        Checksum            inChecksum,
        size_t              inChecksumStartPos)
    {
        if (inBlockSeqLen < 0) {
            panic("log transmitter: invalid block sequence length");
            return;
        }
        Checksum theChecksum = inChecksum;
        if (inChecksumStartPos <= inBlockLen) {
            theChecksum = ComputeBlockChecksum(
                theChecksum,
                inBlockPtr + inChecksumStartPos,
                inBlockLen - inChecksumStartPos
            );
        }
        // Block sequence is at the end of the header, and is part of the
        // checksum.
        char* const theSeqEndPtr = mSeqBuf + kSeqBufSize;
        char*       thePtr       = theSeqEndPtr;
        *--thePtr = '\n';
        thePtr = IntToHexString(inBlockSeqLen, thePtr);
        *--thePtr = ' ';
        thePtr = IntToHexString(inBlockSeq.mLogSeq, thePtr);
        *--thePtr = ' ';
        thePtr = IntToHexString(inBlockSeq.mViewSeq, thePtr);
        *--thePtr = ' ';
        thePtr = IntToHexString(inBlockSeq.mEpochSeq, thePtr);
        *--thePtr = ' ';
        thePtr = IntToHexString(mFileSystemId, thePtr);
        // Non empty block checksum includes leading '\n'
        const int theChecksumFrontLen = 0 < inBlockLen ? 1 : 0;
        theChecksum = ChecksumBlocksCombine(
            ComputeBlockChecksum(
                thePtr,
                theSeqEndPtr - thePtr - theChecksumFrontLen),
            theChecksum,
            inBlockLen + theChecksumFrontLen
        );
        const char* const theSeqPtr   = thePtr;
        const int         theBlockLen =
            (int)(theSeqEndPtr - theSeqPtr) + max(0, (int)inBlockLen);
        char* const theEndPtr = mTmpBuf + kTmpBufSize;
        thePtr = theEndPtr;
        *--thePtr = ' ';
        thePtr = IntToHexString(theBlockLen, thePtr);
        *--thePtr = ':';
        *--thePtr = 'l';
        inBuffer.CopyIn(thePtr, (int)(theEndPtr - thePtr));
        thePtr = theEndPtr;
        *--thePtr = '\n';
        *--thePtr = '\r';
        *--thePtr = '\n';
        *--thePtr = '\r';
        thePtr = IntToHexString(theChecksum, thePtr);
        inBuffer.CopyIn(thePtr, (int)(theEndPtr - thePtr));
        inBuffer.CopyIn(theSeqPtr, (int)(theSeqEndPtr - theSeqPtr));
        inBuffer.CopyIn(inBlockPtr, (int)inBlockLen);
    }
    bool IsUp() const
        { return mUpFlag; }
    void Update(
        Transmitter& inTransmitter);
    int GetAuthType() const
        { return mAuthType; }
    void QueueVrRequest(
        MetaVrRequest& inVrReq,
        NodeId         inNodeId);
    int Update(
        MetaVrSM& inMetaVrSM);
    void GetStatus(
        StatusReporter& inReporter);
    MetaVrLogSeq GetLastLogSeq() const
    {
        return (mMetaVrSMPtr ? mMetaVrSMPtr->GetLastLogSeq() :
            MetaVrLogSeq());
    }
    bool Init(
        MetaVrHello&          inHello,
        const ServerLocation& inPeer)
    {
        return (mMetaVrSMPtr &&
            mMetaVrSMPtr->Init(inHello, inPeer, mTransmitter));
    }
    NodeId GetNodeId() const
        { return mNodeId; }
    void Deleted(
        Transmitter& inTransmitter);
    void Suspend(
        bool inFlag);
    void ScheduleHelloTransmit();
    void ScheduleHeartbeatTransmit();
private:
    typedef Properties::String String;
    enum { kTmpBufSize = 2 + 1 + sizeof(seq_t) * 2 + 4 };
    enum { kSeqBufSize = 5 * kTmpBufSize };

    LogTransmitter& mTransmitter;
    NetManager&     mNetManager;
    int             mRetryInterval;
    int             mMaxReadAhead;
    int             mHeartbeatInterval;
    int             mMinAckToCommit;
    int             mMaxPending;
    int             mCompactionInterval;
    MetaVrLogSeq    mCommitted;
    int             mAuthType;
    string          mAuthTypeStr;
    CommitObserver& mCommitObserver;
    int             mIdsCount;
    NodeId          mNodeId;
    bool            mSendingFlag;
    bool            mPendingUpdateFlag;
    bool            mUpFlag;
    bool            mSuspendedFlag;
    int64_t         mFileSystemId;
    MetaVrSM*       mMetaVrSMPtr;
    string          mTransmitterAuthParamsPrefix;
    Properties      mTransmitterAuthParams;
    int             mTransmittersCount;
    Transmitter*    mTransmittersPtr[1];
    char            mParseBuffer[MAX_RPC_HEADER_LEN];
    char            mTmpBuf[kTmpBufSize + 1];
    char            mSeqBuf[kSeqBufSize + 1];

    void Insert(
        Transmitter& inTransmitter);
    void EndOfTransmit();
    void Update();
    int StartTransmitters(
        ClientAuthContext* inAuthCtxPtr);

private:
    Impl(
        const Impl& inImpl);
    Impl& operator=(
        const Impl& inImpl);
};

class LogTransmitter::Impl::Transmitter :
    public KfsCallbackObj,
    public ITimeout
{
public:
    typedef Impl::List   List;
    typedef Impl::NodeId NodeId;

    Transmitter(
        Impl&                 inImpl,
        const ServerLocation& inServer,
        NodeId                inNodeId,
        bool                  inActiveFlag,
        const MetaVrLogSeq&   inLastLogSeq)
        : KfsCallbackObj(),
          mImpl(inImpl),
          mServer(inServer),
          mPendingSend(),
          mBlocksQueue(),
          mConnectionPtr(),
          mAuthenticateOpPtr(0),
          mVrOpPtr(0),
          mVrOpSeq(-1),
          mNextSeq(mImpl.RandomSeq()),
          mRecursionCount(0),
          mCompactBlockCount(0),
          mHeartbeatSendTimeoutCount(0),
          mAuthContext(),
          mAuthRequestCtx(),
          mLastSentBlockSeq(inLastLogSeq),
          mAckBlockSeq(),
          mAckBlockFlags(0),
          mReplyProps(),
          mIstream(),
          mOstream(),
          mSleepingFlag(false),
          mReceivedIdFlag(false),
          mActiveFlag(inActiveFlag),
          mSendHelloFlag(false),
          mMetaVrHello(*(new MetaVrHello())),
          mReceivedId(-1),
          mPrimaryNodeId(-1),
          mId(inNodeId),
          mPeer()
    {
        SET_HANDLER(this, &Transmitter::HandleEvent);
        List::Init(*this);
    }
    ~Transmitter()
    {
        QCRTASSERT(mRecursionCount == 0);
        Transmitter::Shutdown();
        MetaRequest::Release(mAuthenticateOpPtr);
        if (mSleepingFlag) {
            mImpl.GetNetManager().UnRegisterTimeoutHandler(this);
        }
        VrDisconnect();
        MetaRequest::Release(&mMetaVrHello);
        mImpl.Deleted(*this);
    }
    int SetParameters(
        ClientAuthContext* inAuthCtxPtr,
        const char*        inParamPrefixPtr,
        const Properties&  inParameters,
        string&            outErrMsg)
    {
        const bool kVerifyFlag = true;
        return mAuthContext.SetParameters(
            inParamPrefixPtr,
            inParameters,
            inAuthCtxPtr,
            &outErrMsg,
            kVerifyFlag
        );
    }
    void QueueVrRequest(
        MetaVrRequest& inReq)
    {
        if (! mPendingSend.IsEmpty() || mVrOpPtr ||
                0 <= mMetaVrHello.opSeqno) {
            KFS_LOG_STREAM_DEBUG <<
                mServer <<
                " queue VR request:"
                " pending: "   << mPendingSend.BytesConsumable() <<
                " hello seq: " << mMetaVrHello.opSeqno <<
                " cancel: "    << MetaRequest::ShowReq(mVrOpPtr) <<
            KFS_LOG_EOM;
            Shutdown();
            Reset("queue VR request");
        }
        if (mVrOpPtr) {
            panic("log transmitter: invalid Vr op");
            MetaRequest::Release(mVrOpPtr);
        }
        inReq.Ref();
        mVrOpSeq = -1;
        mVrOpPtr = &inReq;
        if (mConnectionPtr) {
            if (! mAuthenticateOpPtr) {
                StartSend();
            }
        } else {
            Start();
        }
    }
    void Start()
    {
        if (! mConnectionPtr && ! mSleepingFlag) {
            Connect();
            SendHeartbeat();
        }
    }
    int HandleEvent(
        int   inType,
        void* inDataPtr)
    {
        mRecursionCount++;
        QCASSERT(0 < mRecursionCount);
        switch (inType) {
            case EVENT_NET_READ:
                QCASSERT(&mConnectionPtr->GetInBuffer() == inDataPtr);
                HandleRead();
                break;
            case EVENT_NET_WROTE:
                mHeartbeatSendTimeoutCount = 0;
                break;
            case EVENT_CMD_DONE:
                if (! inDataPtr) {
                    panic("log transmitter: invalid null command completion");
                    break;
                }
                HandleCmdDone(*reinterpret_cast<MetaRequest*>(inDataPtr));
                break;
            case EVENT_NET_ERROR:
                if (HandleSslShutdown()) {
                    break;
                }
                Error("network error");
                break;
            case EVENT_INACTIVITY_TIMEOUT:
                if (SendHeartbeat()) {
                    break;
                }
                if (++mHeartbeatSendTimeoutCount <
                        Impl::kHeartbeatsPerTimeoutInterval) {
                    break;
                }
                Error("connection timed out");
                break;
            default:
                panic("log transmitter: unexpected event");
                break;
        }
        if (mRecursionCount <= 1) {
            if (mConnectionPtr && mConnectionPtr->IsGood()) {
                mConnectionPtr->StartFlush();
            } else if (mConnectionPtr) {
                Error();
            }
            if (mConnectionPtr && ! mAuthenticateOpPtr) {
                mConnectionPtr->SetMaxReadAhead(mImpl.GetMaxReadAhead());
                mConnectionPtr->SetInactivityTimeout(
                    mImpl.GetHeartbeatInterval());
            }
        }
        mRecursionCount--;
        QCASSERT(0 <= mRecursionCount);
        return 0;
    }
    void CloseConnection()
    {
        if (mConnectionPtr) {
            mConnectionPtr->Close();
            mConnectionPtr.reset();
        }
        NodeId const thePrevPrimaryId = mPrimaryNodeId;
        mPrimaryNodeId = -1;
        MetaRequest::Release(mAuthenticateOpPtr);
        mAuthenticateOpPtr = 0;
        AdvancePendingQueue();
        const MetaVrLogSeq thePrevAckSeq = mAckBlockSeq;
        mAckBlockSeq = MetaVrLogSeq();
        if (mSleepingFlag) {
            mSleepingFlag = false;
            mImpl.GetNetManager().UnRegisterTimeoutHandler(this);
        }
        mPeer.port = -1;
        mPeer.hostname.clear();
        mSendHelloFlag = true;
        mVrOpSeq = -1;
        mReplyProps.clear();
        UpdateAck(thePrevAckSeq, thePrevPrimaryId);
    }
    void Shutdown()
    {
        CloseConnection();
        VrDisconnect();
        mPeer.port = -1;
        mPeer.hostname.clear();
        mReplyProps.clear();
    }
    const ServerLocation& GetServerLocation() const
        { return mServer; }
    virtual void Timeout()
    {
        if (mSleepingFlag) {
            mSleepingFlag = false;
            mImpl.GetNetManager().UnRegisterTimeoutHandler(this);
        }
        Connect();
    }
    bool SendBlock(
        const MetaVrLogSeq& inBlockSeq,
        int                 inBlockSeqLen,
        IOBuffer&           inBuffer,
        int                 inLen)
    {
        if (inBlockSeq <= mAckBlockSeq ||
                inLen <= 0 ||
                inBlockSeq <= mLastSentBlockSeq ||
                CanBypassSend(inBlockSeq, inBlockSeqLen)) {
            return true;
        }
        if (mImpl.GetMaxPending() < mPendingSend.BytesConsumable()) {
            ExceededMaxPending();
            return false;
        }
        mPendingSend.Copy(&inBuffer, inLen);
        if (mConnectionPtr && ! mAuthenticateOpPtr) {
            mConnectionPtr->GetOutBuffer().Copy(&inBuffer, inLen);
        }
        CompactIfNeeded();
        const bool kHeartbeatFlag = false;
        return FlushBlock(inBlockSeq, inBlockSeqLen, inLen, kHeartbeatFlag);
    }
    bool SendBlock(
        const MetaVrLogSeq& inBlockSeq,
        int                 inBlockSeqLen,
        const char*         inBlockPtr,
        size_t              inBlockLen,
        Checksum            inChecksum,
        size_t              inChecksumStartPos)
    {
        if (inBlockSeq <= mAckBlockSeq || inBlockLen <= 0 ||
                CanBypassSend(inBlockSeq, inBlockSeqLen)) {
            return true;
        }
        const bool kHeartbeatFlag = false;
        return SendBlockSelf(
            inBlockSeq,
            inBlockSeqLen,
            inBlockPtr,
            inBlockLen,
            inChecksum,
            inChecksumStartPos,
            kHeartbeatFlag
        );
    }
    ClientAuthContext& GetAuthCtx()
        { return mAuthContext; }
    NodeId GetId() const
        { return mId; }
    NodeId GetReceivedId() const
        { return mReceivedId; }
    MetaVrLogSeq GetAck() const
        { return mAckBlockSeq; }
    const ServerLocation& GetLocation() const
        { return mServer; }
    bool IsActive() const
        { return mActiveFlag; }
    void SetActive(
        bool inFlag)
        { mActiveFlag = inFlag; }
    NodeId GetPrimaryNodeId() const
        { return mPrimaryNodeId; }
    void ScheduleHelloTransmit()
    {
        if (mSendHelloFlag || ! mConnectionPtr) {
            return;
        }
        mSendHelloFlag = true;
        SendHeartbeat();
    }
    void ScheduleHeartbeatTransmit()
    {
        if (mConnectionPtr) {
            SendHeartbeat();
            return;
        }
        Connect();
    }
private:
    typedef ClientAuthContext::RequestCtx   RequestCtx;
    typedef deque<pair<MetaVrLogSeq, int> > BlocksQueue;

    Impl&              mImpl;
    ServerLocation     mServer;
    IOBuffer           mPendingSend;
    BlocksQueue        mBlocksQueue;
    NetConnectionPtr   mConnectionPtr;
    MetaAuthenticate*  mAuthenticateOpPtr;
    MetaVrRequest*     mVrOpPtr;
    seq_t              mVrOpSeq;
    seq_t              mNextSeq;
    int                mRecursionCount;
    int                mCompactBlockCount;
    int                mHeartbeatSendTimeoutCount;
    ClientAuthContext  mAuthContext;
    RequestCtx         mAuthRequestCtx;
    MetaVrLogSeq       mLastSentBlockSeq;
    MetaVrLogSeq       mAckBlockSeq;
    uint64_t           mAckBlockFlags;
    Properties         mReplyProps;
    IOBuffer::IStream  mIstream;
    IOBuffer::WOStream mOstream;
    bool               mSleepingFlag;
    bool               mReceivedIdFlag;
    bool               mActiveFlag;
    bool               mSendHelloFlag;
    MetaVrHello&       mMetaVrHello;
    NodeId             mReceivedId;
    NodeId             mPrimaryNodeId;
    NodeId const       mId;
    ServerLocation     mPeer;
    Transmitter*       mPrevPtr[1];
    Transmitter*       mNextPtr[1];

    friend class QCDLListOp<Transmitter>;

    void UpdateAck(
        const MetaVrLogSeq& inPrevAck,
        NodeId              inPrevPrimaryNodeId)
    {
        if (mActiveFlag && (inPrevAck != mAckBlockSeq ||
                inPrevPrimaryNodeId != mPrimaryNodeId)) {
            mImpl.Acked(inPrevAck, inPrevPrimaryNodeId, *this);
        }
    }
    bool CanBypassSend(
        const MetaVrLogSeq& inBlockSeq,
        int                 inBlockSeqLen)
    {
        if (inBlockSeqLen <= 0 || mImpl.GetNodeId() != mId || ! mActiveFlag) {
            return false;
        }
        if (inBlockSeq <= mAckBlockSeq) {
            return true;
        }
        mLastSentBlockSeq = inBlockSeq;
        const MetaVrLogSeq thePrevAckSeq = mAckBlockSeq;
        mAckBlockSeq = inBlockSeq;
        if (1 < inBlockSeqLen) {
            mAckBlockSeq.mLogSeq += inBlockSeqLen - 1;
        }
        UpdateAck(thePrevAckSeq, mPrimaryNodeId);
        return true;
    }
    bool SendBlockSelf(
        const MetaVrLogSeq& inBlockSeq,
        int                 inBlockSeqLen,
        const char*         inBlockPtr,
        size_t              inBlockLen,
        Checksum            inChecksum,
        size_t              inChecksumStartPos,
        bool                inHeartbeatFlag)
    {
        if (inBlockSeqLen < 0) {
            panic("log transmitter: invalid block sequence length");
            return false;
        }
        if (mVrOpPtr) {
            return false;
        }
        const int thePos = mPendingSend.BytesConsumable();
        if (mImpl.GetMaxPending() < thePos) {
            ExceededMaxPending();
            return false;
        }
        if (mPendingSend.IsEmpty() || ! mConnectionPtr || mAuthenticateOpPtr) {
            WriteBlock(mPendingSend, inBlockSeq,
                inBlockSeqLen, inBlockPtr, inBlockLen, inChecksum,
                inChecksumStartPos);
        } else {
            IOBuffer theBuffer;
            WriteBlock(theBuffer, inBlockSeq,
                inBlockSeqLen, inBlockPtr, inBlockLen, inChecksum,
                inChecksumStartPos);
            mPendingSend.Move(&theBuffer);
            CompactIfNeeded();
        }
        return FlushBlock(
            inBlockSeq,
            inBlockSeqLen,
            mPendingSend.BytesConsumable() - thePos,
            inHeartbeatFlag
        );
    }
    bool FlushBlock(
        const MetaVrLogSeq& inBlockSeq,
        int                 inBlockSeqLen,
        int                 inLen,
        bool                inHeartbeatFlag)
    {
        if (inBlockSeq < mLastSentBlockSeq || inLen <= 0) {
            panic(
                "log transmitter: "
                "block sequence is invalid: less than last sent, "
                "or invalid length"
            );
            return false;
        }
        mLastSentBlockSeq = inBlockSeq;
        // Allow to cleanup heartbeats by assigning negative / invalid sequence.
        MetaVrLogSeq theEndSeq;
        if (! inHeartbeatFlag) {
            theEndSeq = inBlockSeq;
            if (0 < inBlockSeqLen) {
                theEndSeq.mLogSeq += inBlockSeqLen - 1;
            }
        }
        mBlocksQueue.push_back(make_pair(theEndSeq, inLen));
        if (mRecursionCount <= 0 && ! mAuthenticateOpPtr && mConnectionPtr) {
            if (mConnectionPtr->GetOutBuffer().IsEmpty()) {
                StartSend();
            } else {
                mConnectionPtr->StartFlush();
            }
        }
        return (!! mConnectionPtr);
    }
    void Reset(
        const char*         inErrMsgPtr,
        MsgLogger::LogLevel inLogLevel = MsgLogger::kLogLevelERROR)
    {
        mPendingSend.Clear();
        mBlocksQueue.clear();
        mCompactBlockCount = 0;
        mLastSentBlockSeq  = mImpl.GetLastLogSeq();
        Error(inErrMsgPtr, inLogLevel);
    }
    void ExceededMaxPending()
        { Reset("exceeded max pending send"); }
    void CompactIfNeeded()
    {
        mCompactBlockCount++;
        if (mImpl.GetCompactionInterval() < mCompactBlockCount) {
            mPendingSend.MakeBuffersFull();
            if (mConnectionPtr && ! mAuthenticateOpPtr) {
                mConnectionPtr->GetOutBuffer().MakeBuffersFull();
            }
            mCompactBlockCount = 0;
        }
    }
    void WriteBlock(
        IOBuffer&           inBuffer,
        const MetaVrLogSeq& inBlockSeq,
        int                 inBlockSeqLen,
        const char*         inBlockPtr,
        size_t              inBlockLen,
        Checksum            inChecksum,
        size_t              inChecksumStartPos)
    {
        mImpl.WriteBlock(inBuffer, inBlockSeq,
            inBlockSeqLen, inBlockPtr, inBlockLen, inChecksum,
            inChecksumStartPos);
        if (! mConnectionPtr || mAuthenticateOpPtr) {
            return;
        }
        mConnectionPtr->GetOutBuffer().Copy(
            &inBuffer, inBuffer.BytesConsumable());
    }
    void Connect()
    {
        CloseConnection();
        if (! mImpl.GetNetManager().IsRunning()) {
            return;
        }
        if (! mServer.IsValid()) {
            return;
        }
        mReceivedIdFlag = false;
        TcpSocket* theSocketPtr = new TcpSocket();
        mConnectionPtr.reset(new NetConnection(theSocketPtr, this));
        const bool kNonBlockingFlag = true;
        const int  theErr = theSocketPtr->Connect(mServer, kNonBlockingFlag);
        if (theErr != 0 && theErr != -EINPROGRESS) {
            Error("failed to connect");
            return;
        }
        if (theErr != 0) {
            mConnectionPtr->SetDoingNonblockingConnect();
        }
        mConnectionPtr->SetInactivityTimeout(
            mImpl.GetHeartbeatInterval() * 3 / 2);
        mConnectionPtr->EnableReadIfOverloaded();
        mImpl.GetNetManager().AddConnection(mConnectionPtr);
        if (! Authenticate()) {
            StartSend();
        }
    }
    bool Authenticate()
    {
        if (! mConnectionPtr || ! mAuthContext.IsEnabled()) {
            return false;
        }
        if (mAuthenticateOpPtr) {
            panic("log transmitter: "
                "invalid authenticate invocation: auth is in flight");
            return true;
        }
        mConnectionPtr->SetMaxReadAhead(mImpl.GetMaxReadAhead());
        mAuthenticateOpPtr = new MetaAuthenticate();
        mAuthenticateOpPtr->opSeqno            = GetNextSeq();
        mAuthenticateOpPtr->shortRpcFormatFlag = true;
        string    theErrMsg;
        const int theErr = mAuthContext.Request(
            mImpl.GetAuthType(),
            mAuthenticateOpPtr->sendAuthType,
            mAuthenticateOpPtr->sendContentPtr,
            mAuthenticateOpPtr->sendContentLen,
            mAuthRequestCtx,
            &theErrMsg
        );
        if (theErr) {
            KFS_LOG_STREAM_ERROR <<
                mServer << ": "
                "authentication request failure: " <<
                theErrMsg <<
            KFS_LOG_EOM;
            MetaRequest::Release(mAuthenticateOpPtr);
            mAuthenticateOpPtr = 0;
            Error(theErrMsg.c_str());
            return true;
        }
        KFS_LOG_STREAM_DEBUG <<
            mServer << ": "
            "starting: " <<
            mAuthenticateOpPtr->Show() <<
        KFS_LOG_EOM;
        Request(*mAuthenticateOpPtr);
        return true;
    }
    void HandleRead()
    {
        IOBuffer& theBuf = mConnectionPtr->GetInBuffer();
        if (mAuthenticateOpPtr && 0 < mAuthenticateOpPtr->contentLength) {
            HandleAuthResponse(theBuf);
            if (mAuthenticateOpPtr) {
                return;
            }
        }
        bool theMsgAvailableFlag;
        int  theMsgLen = 0;
        while ((theMsgAvailableFlag = IsMsgAvail(&theBuf, &theMsgLen))) {
            const int theRet = HandleMsg(theBuf, theMsgLen);
            if (theRet < 0) {
                theBuf.Clear();
                Error(mAuthenticateOpPtr ?
                    (mAuthenticateOpPtr->statusMsg.empty() ?
                        "invalid authenticate message" :
                        mAuthenticateOpPtr->statusMsg.c_str()) :
                    "request parse error"
                );
                return;
            }
            if (0 < theRet || ! mConnectionPtr) {
                return; // Need more data, or down
            }
            theMsgLen = 0;
        }
        if (! mAuthenticateOpPtr &&
                MAX_RPC_HEADER_LEN < theBuf.BytesConsumable()) {
            Error("header size exceeds max allowed");
        }
    }
    void HandleAuthResponse(
        IOBuffer& inBuffer)
    {
        if (! mAuthenticateOpPtr || ! mConnectionPtr) {
            panic("log transmitter: "
                "handle auth response: invalid invocation");
            MetaRequest::Release(mAuthenticateOpPtr);
            mAuthenticateOpPtr = 0;
            Error();
            return;
        }
        if (! mAuthenticateOpPtr->contentBuf &&
                0 < mAuthenticateOpPtr->contentLength) {
            mAuthenticateOpPtr->contentBuf =
                new char [mAuthenticateOpPtr->contentLength];
        }
        const int theRem = mAuthenticateOpPtr->Read(inBuffer);
        if (0 < theRem) {
            // Request one byte more to detect extaneous data.
            mConnectionPtr->SetMaxReadAhead(theRem + 1);
            return;
        }
        if (! inBuffer.IsEmpty()) {
            KFS_LOG_STREAM_ERROR <<
                mServer << ": "
                "authentication protocol failure:" <<
                " " << inBuffer.BytesConsumable() <<
                " bytes past authentication response" <<
                " filter: " <<
                    reinterpret_cast<const void*>(mConnectionPtr->GetFilter()) <<
                " cmd: " << mAuthenticateOpPtr->Show() <<
            KFS_LOG_EOM;
            if (! mAuthenticateOpPtr->statusMsg.empty()) {
                mAuthenticateOpPtr->statusMsg += "; ";
            }
            mAuthenticateOpPtr->statusMsg += "invalid extraneous data received";
            mAuthenticateOpPtr->status    = -EINVAL;
        } else if (mAuthenticateOpPtr->status == 0) {
            if (mConnectionPtr->GetFilter()) {
                // Shutdown the current filter.
                mConnectionPtr->Shutdown();
                return;
            }
            mAuthenticateOpPtr->status = mAuthContext.Response(
                mAuthenticateOpPtr->authType,
                mAuthenticateOpPtr->useSslFlag,
                mAuthenticateOpPtr->contentBuf,
                mAuthenticateOpPtr->contentLength,
                *mConnectionPtr,
                mAuthRequestCtx,
                &mAuthenticateOpPtr->statusMsg
            );
        }
        const string theErrMsg = mAuthenticateOpPtr->statusMsg;
        const bool   theOkFlag = mAuthenticateOpPtr->status == 0;
        KFS_LOG_STREAM(theOkFlag ?
                MsgLogger::kLogLevelDEBUG : MsgLogger::kLogLevelERROR) <<
            mServer << ":"
            " finished: " << mAuthenticateOpPtr->Show() <<
            " filter: "   <<
                reinterpret_cast<const void*>(mConnectionPtr->GetFilter()) <<
        KFS_LOG_EOM;
        MetaRequest::Release(mAuthenticateOpPtr);
        mAuthenticateOpPtr = 0;
        if (! theOkFlag) {
            Error(theErrMsg.c_str());
            return;
        }
        StartSend();
    }
    void StartSend()
    {
        if (! mConnectionPtr) {
            return;
        }
        if (mAuthenticateOpPtr) {
            panic("log transmitter: "
                "invalid start send invocation: "
                "authentication is in progress");
            return;
        }
        if (mVrOpPtr) {
            mSendHelloFlag = false;
            mVrOpSeq = GetNextSeq();
            mVrOpPtr->opSeqno = mVrOpSeq;
            Request(*mVrOpPtr);
            return;
        }
        mRecursionCount++;
        if (mSendHelloFlag) {
            SendHello();
        }
        if (mPendingSend.IsEmpty()) {
            SendHeartbeat();
        } else {
            mConnectionPtr->GetOutBuffer().Copy(
                &mPendingSend, mPendingSend.BytesConsumable());
        }
        mRecursionCount--;
        if (mRecursionCount <= 0) {
            mConnectionPtr->StartFlush();
        }
    }
    bool HandleSslShutdown()
    {
        if (mAuthenticateOpPtr &&
                mConnectionPtr &&
                mConnectionPtr->IsGood() &&
                ! mConnectionPtr->GetFilter()) {
            HandleAuthResponse(mConnectionPtr->GetInBuffer());
            return (!! mConnectionPtr);
        }
        return false;
    }
    void SendHello()
    {
        mSendHelloFlag = false;
        mMetaVrHello.shortRpcFormatFlag = true;
        if (mImpl.Init(mMetaVrHello, GetPeerLocation())) {
            mMetaVrHello.opSeqno = GetNextSeq();
            Request(mMetaVrHello);
        }
    }
    bool SendHeartbeat()
    {
        if ((mActiveFlag &&
                mAckBlockSeq.IsValid() &&
                mAckBlockSeq < mLastSentBlockSeq) ||
                ! mBlocksQueue.empty() ||
                mVrOpPtr ||
                mAuthenticateOpPtr) {
            return false;
        }
        // if (mSendHelloFlag) {
        //    SendHello();
        // }
        if (! mLastSentBlockSeq.IsValid()) {
            mLastSentBlockSeq = mImpl.GetLastLogSeq();
        }
        const bool kHeartbeatFlag = true;
        SendBlockSelf(
            mLastSentBlockSeq.IsValid() ?
                mLastSentBlockSeq : MetaVrLogSeq(0, 0, 0),
            0, "", 0, kKfsNullChecksum, 0, kHeartbeatFlag);
        return true;
    }
    int HandleMsg(
        IOBuffer& inBuffer,
        int       inHeaderLen)
    {
        const char* const theHeaderPtr = inBuffer.CopyOutOrGetBufPtr(
            mImpl.GetParseBufferPtr(), inHeaderLen);
        if (2 <= inHeaderLen &&
                (theHeaderPtr[0] & 0xFF) == 'A' &&
                (theHeaderPtr[1] & 0xFF) <= ' ') {
            return HandleAck(theHeaderPtr, inHeaderLen, inBuffer);
        }
        if (3 <= inHeaderLen &&
                (theHeaderPtr[0] & 0xFF) == 'O' &&
                (theHeaderPtr[1] & 0xFF) == 'K' &&
                (theHeaderPtr[2] & 0xFF) <= ' ') {
            return HandleReply(theHeaderPtr, inHeaderLen, inBuffer);
        }
        return HanldeRequest(theHeaderPtr, inHeaderLen, inBuffer);
    }
    void AdvancePendingQueue()
    {
        while (! mBlocksQueue.empty()) {
            const BlocksQueue::value_type& theFront = mBlocksQueue.front();
            if (mAckBlockSeq < theFront.first) {
                break;
            }
            if (mPendingSend.Consume(theFront.second) != theFront.second) {
                panic("log transmitter: "
                    "invalid pending send buffer or queue");
            }
            mBlocksQueue.pop_front();
            if (0 < mCompactBlockCount) {
                mCompactBlockCount--;
            }
        }
    }
    int HandleAck(
        const char* inHeaderPtr,
        int         inHeaderLen,
        IOBuffer&   inBuffer)
    {
        const NodeId       thePrevPrimaryId = mPrimaryNodeId;
        const MetaVrLogSeq thePrevAckSeq    = mAckBlockSeq;
        const char*        thePtr           = inHeaderPtr + 2;
        const char* const  theEndPtr        = thePtr + inHeaderLen;
        if (! mAckBlockSeq.Parse<HexIntParser>(
                    thePtr, theEndPtr - thePtr) ||
                ! HexIntParser::Parse(
                    thePtr, theEndPtr - thePtr, mAckBlockFlags) ||
                ! HexIntParser::Parse(
                    thePtr, theEndPtr - thePtr, mPrimaryNodeId)) {
            MsgLogLines(MsgLogger::kLogLevelERROR,
                "malformed ack: ", inBuffer, inHeaderLen);
            Error("malformed ack");
            return -1;
        }
        if (! mAckBlockSeq.IsValid()) {
            KFS_LOG_STREAM_ERROR <<
                mServer << ": "
                "invalid ack block sequence: " << mAckBlockSeq <<
                " last sent: "                 << mLastSentBlockSeq <<
                " pending: "                   <<
                    mPendingSend.BytesConsumable() <<
                " / "                          << mBlocksQueue.size() <<
            KFS_LOG_EOM;
            Error("invalid ack sequence");
            return -1;
        }
        const bool theHasIdFlag = mAckBlockFlags &
            (uint64_t(1) << kLogBlockAckHasServerIdBit);
        NodeId     theId        = -1;
        if (theHasIdFlag  &&
                (! HexIntParser::Parse(thePtr, theEndPtr - thePtr, theId) ||
                theId < 0)) {
            KFS_LOG_STREAM_ERROR <<
                mServer << ": "
                "missing or invalid server id: " << theId <<
                " last sent: "                   << mLastSentBlockSeq <<
            KFS_LOG_EOM;
            Error("missing or invalid server id");
            return -1;
        }
        while (thePtr < theEndPtr && (*thePtr & 0xFF) <= ' ') {
            thePtr++;
        }
        const char* const theChksumEndPtr = thePtr;
        Checksum theChecksum = 0;
        if (! HexIntParser::Parse(
                thePtr, theEndPtr - thePtr, theChecksum)) {
            KFS_LOG_STREAM_ERROR <<
                mServer << ": "
                "invalid ack checksum: " << theChecksum <<
                " last sent: "           << mLastSentBlockSeq <<
            KFS_LOG_EOM;
            Error("missing or invalid server id");
            return -1;
        }
        const Checksum theComputedChksum = ComputeBlockChecksum(
            inHeaderPtr, theChksumEndPtr - inHeaderPtr);
        if (theComputedChksum != theChecksum) {
            KFS_LOG_STREAM_ERROR <<
                mServer << ": "
                "ack checksum mismatch:"
                " expected: " << theChecksum <<
                " computed: " << theComputedChksum <<
            KFS_LOG_EOM;
            Error("ack checksum mismatch");
            return -1;
        }
        if (! mReceivedIdFlag) {
            mReceivedId = theId;
            if (theHasIdFlag) {
                mReceivedIdFlag = true;
                if (! mActiveFlag && mId != theId) {
                    KFS_LOG_STREAM_NOTICE <<
                        mServer << ": inactive node ack id mismatch:" <<
                        " expected: " << mId <<
                        " actual:: "  << theId <<
                    KFS_LOG_EOM;
                }
            } else {
                const char* const theMsgPtr = "first ack wihout node id";
                KFS_LOG_STREAM_ERROR <<
                    mServer << ": " << theMsgPtr <<
                KFS_LOG_EOM;
                Error(theMsgPtr);
                return -1;
            }
        }
        if (theHasIdFlag && mId != theId) {
            KFS_LOG_STREAM_ERROR <<
                mServer << ": "
                "ack node id mismatch:"
                " expected: " << mId <<
                " actual:: "  << theId <<
            KFS_LOG_EOM;
            Error("ack node id mismatch");
            return -1;
        }
        KFS_LOG_STREAM_DEBUG <<
            mServer << ":"
            " log recv id: " << theId <<
            " / "            << mId <<
            " primary: "     << mPrimaryNodeId <<
            " ack: "         << thePrevAckSeq <<
            " => "           << mAckBlockSeq <<
            " sent: "        << mLastSentBlockSeq <<
            " pending:"
            " blocks: "      << mBlocksQueue.size() <<
            " bytes: "       << mPendingSend.BytesConsumable() <<
        KFS_LOG_EOM;
        AdvancePendingQueue();
        inBuffer.Consume(inHeaderLen);
        UpdateAck(thePrevAckSeq, thePrevPrimaryId);
        if (mConnectionPtr && ! mAuthenticateOpPtr &&
                (mAckBlockFlags &
                    (uint64_t(1) << kLogBlockAckReAuthFlagBit)) != 0) {
            KFS_LOG_STREAM_DEBUG <<
                mServer << ": "
                "re-authentication requested" <<
            KFS_LOG_EOM;
            Authenticate();
        }
        return (mConnectionPtr ? 0 : -1);
    }
    const ServerLocation& GetPeerLocation()
    {
        if (! mPeer.IsValid() && mConnectionPtr &&
                0 != mConnectionPtr->GetPeerLocation(mPeer)) {
            mPeer.port = -1;
            mPeer.hostname.clear();
        }
        return mPeer;
    }
    int HandleReply(
        const char* inHeaderPtr,
        int         inHeaderLen,
        IOBuffer&   inBuffer)
    {
        mReplyProps.clear();
        mReplyProps.setIntBase(16);
        if (mReplyProps.loadProperties(
                inHeaderPtr, inHeaderLen, (char)':') != 0) {
            MsgLogLines(MsgLogger::kLogLevelERROR,
                "malformed reply: ", inBuffer, inHeaderLen);
            Error("malformed reply");
            return -1;
        }
        // For now only handle authentication response.
        seq_t const theSeq = mReplyProps.getValue("c", seq_t(-1));
        if ((mVrOpPtr && 0 <= mVrOpSeq && theSeq != mVrOpSeq) ||
                (0 <= mMetaVrHello.opSeqno && theSeq != mMetaVrHello.opSeqno) ||
                (mAuthenticateOpPtr && theSeq != mAuthenticateOpPtr->opSeqno)) {
            KFS_LOG_STREAM_ERROR <<
                mServer << ": "
                "unexpected reply, authentication: " <<
                MetaRequest::ShowReq(mAuthenticateOpPtr) <<
            KFS_LOG_EOM;
            MsgLogLines(MsgLogger::kLogLevelERROR,
                "unexpected reply: ", inBuffer, inHeaderLen);
            Error("unexpected reply");
            return -1;
        }
        inBuffer.Consume(inHeaderLen);
        if (mAuthenticateOpPtr) {
            mAuthenticateOpPtr->contentLength         =
                mReplyProps.getValue("l", 0);
            mAuthenticateOpPtr->authType              =
                mReplyProps.getValue("A", int(kAuthenticationTypeUndef));
            mAuthenticateOpPtr->useSslFlag            =
                mReplyProps.getValue("US", 0) != 0;
            int64_t theCurrentTime                    =
                mReplyProps.getValue("CT", int64_t(-1));
            mAuthenticateOpPtr->sessionExpirationTime =
                mReplyProps.getValue("ET", int64_t(-1));
            KFS_LOG_STREAM_DEBUG <<
                mServer << ": "
                "authentication reply:"
                " cur time: "   << theCurrentTime <<
                " delta: "      << (TimeNow() - theCurrentTime) <<
                " expires in: " <<
                    (mAuthenticateOpPtr->sessionExpirationTime -
                        theCurrentTime) <<
            KFS_LOG_EOM;
            HandleAuthResponse(inBuffer);
        } else if (theSeq == mMetaVrHello.opSeqno) {
            KFS_LOG_STREAM_DEBUG <<
                mServer << ": "
                "-seq: "    << theSeq <<
                " status: " << mMetaVrHello.status <<
                " "         << mMetaVrHello.statusMsg <<
                " "         << mMetaVrHello.Show() <<
            KFS_LOG_EOM;
            mMetaVrHello.HandleResponse(theSeq, mReplyProps, mId,
                GetPeerLocation());
            mMetaVrHello.opSeqno = -1;
            if (0 != mMetaVrHello.status) {
                Error(mMetaVrHello.statusMsg.empty() ?
                    "VR hello error" : mMetaVrHello.statusMsg.c_str());
                mReplyProps.clear();
                return -1;
            }
        } else {
            VrUpdate(theSeq);
        }
        mReplyProps.clear();
        return (mConnectionPtr ? 0 : -1);
    }
    int HanldeRequest(
        const char* inHeaderPtr,
        int         inHeaderLen,
        IOBuffer&   inBuffer)
    {
        // No request handling for now.
        MsgLogLines(MsgLogger::kLogLevelERROR,
            "invalid response: ", inBuffer, inHeaderLen);
        Error("invalid response");
        return -1;
    }
    void HandleCmdDone(
        MetaRequest& inReq)
    {
        KFS_LOG_STREAM_FATAL <<
            mServer << ": "
            "unexpected invocation: " << inReq.Show() <<
        KFS_LOG_EOM;
        panic("LogTransmitter::Impl::Transmitter::HandleCmdDone "
            "unexpected invocation");
    }
    seq_t GetNextSeq()
        { return ++mNextSeq; }
    void Request(
        MetaRequest& inReq)
    {
        // For now authentication or Vr ops.
        if (&inReq != mAuthenticateOpPtr && &inReq != mVrOpPtr &&
                &inReq != &mMetaVrHello) {
            panic("LogTransmitter::Impl::Transmitter: invalid request");
            return;
        }
        if (! mConnectionPtr) {
            return;
        }
        KFS_LOG_STREAM_DEBUG <<
            mServer << ":"
            " id: "   << mId <<
            " +seq: " << inReq.opSeqno <<
            " "      << inReq.Show() <<
        KFS_LOG_EOM;
        IOBuffer& theBuf = mConnectionPtr->GetOutBuffer();
        ReqOstream theStream(mOstream.Set(theBuf));
        if (&inReq == mVrOpPtr) {
            mVrOpPtr->Request(theStream);
        } else if (&inReq == &mMetaVrHello) {
            mMetaVrHello.Request(theStream);
        } else {
            mAuthenticateOpPtr->Request(theStream);
        }
        mOstream.Reset();
        if (mRecursionCount <= 0) {
            mConnectionPtr->StartFlush();
        }
    }
    void Error(
        const char*          inMsgPtr   = 0,
        MsgLogger::LogLevel  inLogLevel = MsgLogger::kLogLevelERROR)
    {
        if (! mConnectionPtr) {
            return;
        }
        KFS_LOG_STREAM(inLogLevel) <<
            mServer << ": " <<
            (inMsgPtr ? inMsgPtr : "network error") <<
            " socket error: " << mConnectionPtr->GetErrorMsg() <<
            " vr:"
            " seq: "          << mVrOpSeq <<
            " op: "           << MetaRequest::ShowReq(mVrOpPtr) <<
            " hello seq: "    << mMetaVrHello.opSeqno <<
            " pending: "
            " blocks: "       << mBlocksQueue.size() <<
            " bytes: "        << mPendingSend.BytesConsumable() <<
        KFS_LOG_EOM;
        const NodeId thePrevPrimaryId = mPrimaryNodeId;
        mPrimaryNodeId = -1;
        mConnectionPtr->Close();
        mConnectionPtr.reset();
        MetaRequest::Release(mAuthenticateOpPtr);
        mAuthenticateOpPtr   = 0;
        AdvancePendingQueue();
        const MetaVrLogSeq thePrevAck = mAckBlockSeq;
        mAckBlockSeq = MetaVrLogSeq();
        VrDisconnect();
        UpdateAck(thePrevAck, thePrevPrimaryId);
        if (mSleepingFlag) {
            return;
        }
        mSleepingFlag = true;
        SetTimeoutInterval(mImpl.GetRetryInterval());
        mImpl.GetNetManager().RegisterTimeoutHandler(this);
    }
    void VrUpdate(
        seq_t inSeq)
    {
        if (! mVrOpPtr) {
            return;
        }
        MetaVrRequest& theReq = *mVrOpPtr;
        if (inSeq != mVrOpSeq) {
            mReplyProps.clear();
        }
        mVrOpSeq = -1;
        mVrOpPtr = 0;
        theReq.HandleResponse(inSeq, mReplyProps, mId, GetPeerLocation());
        MetaRequest::Release(&theReq);
    }
    void VrDisconnect()
    {
        if (0 <= mMetaVrHello.opSeqno) {
            mMetaVrHello.opSeqno = -1;
            mReplyProps.clear();
            mMetaVrHello.HandleResponse(mMetaVrHello.opSeqno, mReplyProps, mId,
                GetPeerLocation());
        }
        VrUpdate(-1);
    }
    void MsgLogLines(
        MsgLogger::LogLevel inLogLevel,
        const char*         inPrefixPtr,
        IOBuffer&           inBuffer,
        int                 inBufLen,
        int                 inMaxLines = 64)
    {
        const char* const thePrefixPtr = inPrefixPtr ? inPrefixPtr : "";
        istream&          theStream    = mIstream.Set(inBuffer, inBufLen);
        int               theRemCnt    = inMaxLines;
        string            theLine;
        while (--theRemCnt >= 0 && getline(theStream, theLine)) {
            string::iterator theIt = theLine.end();
            if (theIt != theLine.begin() && *--theIt <= ' ') {
                theLine.erase(theIt);
            }
            KFS_LOG_STREAM(inLogLevel) <<
                thePrefixPtr << theLine <<
            KFS_LOG_EOM;
        }
        mIstream.Reset();
    }
    time_t TimeNow()
        { return mImpl.GetNetManager().Now(); }
private:
    Transmitter(
        const Transmitter& inTransmitter);
    Transmitter& operator=(
        const Transmitter& inTransmitter);
};

    int
LogTransmitter::Impl::SetParameters(
    const char*       inParamPrefixPtr,
    const Properties& inParameters)
{
    Properties::String theParamName;
    if (inParamPrefixPtr) {
        theParamName.Append(inParamPrefixPtr);
    }
    const size_t thePrefixLen = theParamName.GetSize();
    mRetryInterval = inParameters.getValue(
        theParamName.Truncate(thePrefixLen).Append(
        "retryInterval"), mRetryInterval);
    mMaxReadAhead = max(512, min(64 << 20, inParameters.getValue(
        theParamName.Truncate(thePrefixLen).Append(
        "maxReadAhead"), mMaxReadAhead)));
    mMaxPending = inParameters.getValue(
        theParamName.Truncate(thePrefixLen).Append(
        "maxPending"), mMaxPending);
    mCompactionInterval = inParameters.getValue(
        theParamName.Truncate(thePrefixLen).Append(
        "compactionInterval"), mCompactionInterval);
    mAuthTypeStr = inParameters.getValue(
        theParamName.Truncate(thePrefixLen).Append(
        "authType"), mAuthTypeStr);
    const char* thePtr = mAuthTypeStr.c_str();
    mAuthType = 0;
    while (*thePtr != 0) {
        while (*thePtr != 0 && (*thePtr & 0xFF) <= ' ') {
            thePtr++;
        }
        const char* theStartPtr = thePtr;
        while (' ' < (*thePtr & 0xFF)) {
            thePtr++;
        }
        const size_t theLen = thePtr - theStartPtr;
        if (theLen == 3) {
            if (memcmp("Krb5", theStartPtr, theLen) == 0) {
                mAuthType |= kAuthenticationTypeKrb5;
            } else if (memcmp("PSK", theStartPtr, theLen) == 0) {
                mAuthType |= kAuthenticationTypeKrb5;
            }
        } else if (theLen == 4 && memcmp("X509", theStartPtr, theLen) == 0) {
            mAuthType |= kAuthenticationTypeX509;
        }
    }
    mTransmitterAuthParamsPrefix =
        theParamName.Truncate(thePrefixLen).Append("auth.").GetStr();
    inParameters.copyWithPrefix(
        mTransmitterAuthParamsPrefix, mTransmitterAuthParams);
    return StartTransmitters(0);
}

    int
LogTransmitter::Impl::StartTransmitters(
    ClientAuthContext* inAuthCtxPtr)
{
    if (List::IsEmpty(mTransmittersPtr)) {
        return 0;
    }
    const bool         theStartFlag     = ! mSuspendedFlag &&
        mMetaVrSMPtr && mMetaVrSMPtr->IsPrimary();
    const char* const  theAuthPrefixPtr = mTransmitterAuthParamsPrefix.c_str();
    ClientAuthContext* theAuthCtxPtr    = inAuthCtxPtr ? inAuthCtxPtr :
        &(List::Front(mTransmittersPtr)->GetAuthCtx());
    int                theRet           = 0;
    List::Iterator     theIt(mTransmittersPtr);
    Transmitter*       theTPtr;
    while ((theTPtr = theIt.Next())) {
        string    theErrMsg;
        const int theErr = theTPtr->SetParameters(
            theAuthCtxPtr, theAuthPrefixPtr, mTransmitterAuthParams, theErrMsg);
        if (0 != theErr) {
            if (theErrMsg.empty()) {
                theErrMsg = QCUtils::SysError(theErr,
                    "setting authentication parameters error");
            }
            KFS_LOG_STREAM_ERROR <<
                theTPtr->GetServerLocation() << ": " <<
                theErrMsg <<
            KFS_LOG_EOM;
            if (0 == theRet) {
                theRet = theErr;
            }
        } else if (theStartFlag) {
            theTPtr->Start();
        }
        if (! theAuthCtxPtr) {
            theAuthCtxPtr = &theTPtr->GetAuthCtx();
        }
    }
    return theRet;
}

    void
LogTransmitter::Impl::Shutdown()
{
    Transmitter* thePtr;
    while ((thePtr = List::PopBack(mTransmittersPtr))) {
        delete thePtr;
    }
}

    void
LogTransmitter::Impl::Deleted(
    Transmitter& inTransmitter)
{
    if (List::IsInList(mTransmittersPtr, inTransmitter)) {
        panic("log transmitter: invalid transmitter delete attempt");
    }
}

    void
LogTransmitter::Impl::Insert(
    LogTransmitter::Impl::Transmitter& inTransmitter)
{
    Transmitter* const theHeadPtr = List::Front(mTransmittersPtr);
    if (! theHeadPtr) {
        List::PushFront(mTransmittersPtr, inTransmitter);
        return;
    }
    // Insertion sort.
    const NodeId theId  = inTransmitter.GetId();
    Transmitter* thePtr = theHeadPtr;
    while (theId < thePtr->GetId()) {
        if (theHeadPtr == (thePtr = &List::GetNext(*thePtr))) {
            List::PushBack(mTransmittersPtr, inTransmitter);
            return;
        }
    }
    if (thePtr == theHeadPtr) {
        List::PushFront(mTransmittersPtr, inTransmitter);
    } else {
        QCDLListOp<Transmitter>::Insert(inTransmitter, List::GetPrev(*thePtr));
    }
}

    void
LogTransmitter::Impl::Acked(
    const MetaVrLogSeq&                inPrevAck,
    LogTransmitter::Impl::NodeId       inPrimaryNodeId,
    LogTransmitter::Impl::Transmitter& inTransmitter)
{
    if (! inTransmitter.IsActive() || List::IsEmpty(mTransmittersPtr)) {
        return;
    }
    const NodeId thePrimaryId = inTransmitter.GetPrimaryNodeId();
    if (inPrimaryNodeId != thePrimaryId && 0 < thePrimaryId && mMetaVrSMPtr) {
        if (! mMetaVrSMPtr->ValidateAckPrimaryId(
                inTransmitter.GetId(), thePrimaryId)) {
            return;
        }
    }
    const NodeId theCurPrimaryId =  mMetaVrSMPtr ?
        mMetaVrSMPtr->GetPrimaryNodeId() : NodeId(-1);
    const MetaVrLogSeq theAck    = inTransmitter.GetAck();
    if (mCommitted < theAck && 0 <= theCurPrimaryId) {
        NodeId             thePrevId    = -1;
        int                theAckAdvCnt = 0;
        MetaVrLogSeq       theCommitted = theAck;
        MetaVrLogSeq       theCurMax    = mCommitted;
        List::Iterator     theIt(mTransmittersPtr);
        const Transmitter* thePtr;
        while ((thePtr = theIt.Next())) {
            if (! thePtr->IsActive()) {
                continue;
            }
            const MetaVrLogSeq theCurAck = thePtr->GetAck();
            if (! theCurAck.IsValid()) {
                continue;
            }
            const NodeId theId = thePtr->GetId();
            if (theCurPrimaryId != thePtr->GetPrimaryNodeId()) {
                continue;
            }
            if (theId == thePrevId) {
                theCurMax = max(theCurMax, theCurAck);
            } else {
                if (mCommitted < theCurMax) {
                    theCommitted = min(theCommitted, theCurMax);
                    theAckAdvCnt++;
                }
                theCurMax = theCurAck;
                thePrevId = theId;
            }
        }
        if (mCommitted < theCurMax) {
            theCommitted = min(theCommitted, theCurMax);
            theAckAdvCnt++;
        }
        if (mMinAckToCommit <= theAckAdvCnt) {
            mCommitted = theCommitted;
            mCommitObserver.Notify(mCommitted);
        }
    }
    if (inPrevAck.IsValid() != theAck.IsValid() || theCurPrimaryId < 0 ||
            inPrimaryNodeId != thePrimaryId) {
        Update(inTransmitter);
    }
}

    int
LogTransmitter::Impl::TransmitBlock(
    const MetaVrLogSeq&            inBlockSeq,
    int                            inBlockSeqLen,
    const char*                    inBlockPtr,
    size_t                         inBlockLen,
    LogTransmitter::Impl::Checksum inChecksum,
    size_t                         inChecksumStartPos)
{
    if (inBlockSeqLen < 0) {
        return -EINVAL;
    }
    if (List::IsEmpty(mTransmittersPtr)) {
        mCommitted = inBlockSeq;
        mCommitObserver.Notify(mCommitted);
        return 0;
    }
    if (inBlockLen <= 0) {
        return 0;
    }
    mSendingFlag = true;
    int          theCnt = 0;
    Transmitter* thePtr;
    if (List::Front(mTransmittersPtr) == List::Back(mTransmittersPtr)) {
        thePtr = List::Front(mTransmittersPtr);
        const NodeId theId = thePtr->GetId();
        if (thePtr->SendBlock(
                    inBlockSeq, inBlockSeqLen,
                    inBlockPtr, inBlockLen, inChecksum, inChecksumStartPos) &&
                0 <= theId && thePtr->IsActive()) {
            theCnt++;
        }
    } else {
        IOBuffer theBuffer;
        WriteBlock(theBuffer, inBlockSeq, inBlockSeqLen,
            inBlockPtr, inBlockLen, inChecksum, inChecksumStartPos);
        NodeId         thePrevId = -1;
        List::Iterator theIt(mTransmittersPtr);
        while ((thePtr = theIt.Next())) {
            const NodeId theId = thePtr->GetId();
            if (thePtr->SendBlock(
                        inBlockSeq, inBlockSeqLen,
                        theBuffer, theBuffer.BytesConsumable())) {
                if (0 <= theId && theId != thePrevId && thePtr->IsActive()) {
                    theCnt++;
                }
                thePrevId = theId;
            }
        }
    }
    EndOfTransmit();
    if (mMinAckToCommit <= 0 && mCommitted < inBlockSeq) {
        mCommitted = inBlockSeq;
        mCommitObserver.Notify(mCommitted);
    }
    return (theCnt < mMinAckToCommit ? -EIO : 0);
}

    void
LogTransmitter::Impl::EndOfTransmit()
{
    if (! mSendingFlag) {
        panic("log transmitter: invalid end of transmit invocation");
    }
    mSendingFlag = false;
    if (mPendingUpdateFlag) {
        Update();
    }
}

    void
LogTransmitter::Impl::Update(
    LogTransmitter::Impl::Transmitter& /* inTransmitter */)
{
    Update();
}

    void
LogTransmitter::Impl::Update()
{
    if (mSendingFlag) {
        mPendingUpdateFlag = true;
        return;
    }
    mPendingUpdateFlag = false;
    int            theIdCnt     = 0;
    int            theUpCnt     = 0;
    int            theIdUpCnt   = 0;
    int            theTotalCnt  = 0;
    int            thePrevAllId = -1;
    NodeId         thePrevId    = -1;
    MetaVrLogSeq   theMinAck;
    MetaVrLogSeq   theMaxAck;
    List::Iterator theIt(mTransmittersPtr);
    Transmitter*   thePtr;
    const NodeId   theCurPrimaryId =  mMetaVrSMPtr ?
        mMetaVrSMPtr->GetPrimaryNodeId() : NodeId(-1);
    if (0 <= theCurPrimaryId) {
        while ((thePtr = theIt.Next())) {
            const NodeId       theId  = thePtr->GetId();
            const MetaVrLogSeq theAck = thePtr->GetAck();
            if (0 <= theId && theId != thePrevAllId) {
                theIdCnt++;
                thePrevAllId = theId;
            }
            if (thePtr->IsActive() && theAck.IsValid() &&
                    theCurPrimaryId == thePtr->GetPrimaryNodeId()) {
                theUpCnt++;
                if (theId != thePrevId) {
                    theIdUpCnt++;
                }
                if (theMinAck.IsValid()) {
                    theMinAck = min(theMinAck, theAck);
                    theMaxAck = max(theMaxAck, theAck);
                } else {
                    theMinAck = theAck;
                    theMaxAck = theAck;
                }
                thePrevId = theId;
            }
            theTotalCnt++;
        }
    }
    const bool theUpFlag     = mMinAckToCommit <= theIdUpCnt;
    const bool theNotifyFlag = theUpFlag != mUpFlag;
    KFS_LOG_STREAM(theNotifyFlag ?
            MsgLogger::kLogLevelINFO : MsgLogger::kLogLevelDEBUG) <<
        "update:"
        " primary: "     << theCurPrimaryId <<
        " tranmitters: " << theTotalCnt <<
        " up: "          << theUpCnt <<
        " ids up: "      << theIdUpCnt <<
        " quorum: "      << mMinAckToCommit <<
        " committed: "   << mCommitted <<
        " ack: ["        << theMinAck <<
        ","              << theMaxAck << "]"
        " ids: "         << mIdsCount <<
        " => "           << theIdCnt <<
        " up: "          << mUpFlag <<
        " => "           << theUpFlag <<
    KFS_LOG_EOM;
    mIdsCount = theIdCnt;
    mUpFlag   = theUpFlag;
    if (theNotifyFlag) {
        mCommitObserver.Notify(mCommitted);
    }
}

    void
LogTransmitter::Impl::GetStatus(
    LogTransmitter::StatusReporter& inReporter)
{
    List::Iterator theIt(mTransmittersPtr);
    Transmitter*   thePtr;
    while ((thePtr = theIt.Next())) {
        if (! inReporter.Report(
                thePtr->GetLocation(),
                thePtr->GetId(),
                thePtr->IsActive(),
                thePtr->GetReceivedId(),
                thePtr->GetPrimaryNodeId(),
                thePtr->GetAck(),
                mCommitted)) {
            break;
        }
    }
}

    void
LogTransmitter::Impl::QueueVrRequest(
    MetaVrRequest&               inVrReq,
    LogTransmitter::Impl::NodeId inNodeId)
{
    inVrReq.shortRpcFormatFlag = true;
    List::Iterator theIt(mTransmittersPtr);
    Transmitter*   thePtr;
    while ((thePtr = theIt.Next())) {
        if (inNodeId < 0 || thePtr->GetId() == inNodeId) {
            thePtr->QueueVrRequest(inVrReq);
            if (0 <= inNodeId) {
                break;
            }
        }
    }
}

    int
LogTransmitter::Impl::Update(
    MetaVrSM& inMetaVrSM)
{
    mMetaVrSMPtr = &inMetaVrSM;
    const MetaVrLogSeq       theLastLogSeq = inMetaVrSM.GetLastLogSeq();
    const Config&            theConfig     = inMetaVrSM.GetConfig();
    const Config::Nodes&     theNodes      = theConfig.GetNodes();
    ClientAuthContext* const theAuthCtxPtr = List::IsEmpty(mTransmittersPtr) ?
        0 : &(List::Front(mTransmittersPtr)->GetAuthCtx());
    Transmitter*             theTransmittersPtr[1];
    theTransmittersPtr[0] = mTransmittersPtr[0];
    mTransmittersPtr[0] = 0;
    int theTransmittersCount = 0;
    SetHeartbeatInterval(theConfig.GetPrimaryTimeout());
    for (Config::Nodes::const_iterator theIt = theNodes.begin();
            theNodes.end() != theIt;
            ++theIt) {
        const Config::NodeId     theId        = theIt->first;
        const Config::Node&      theNode      = theIt->second;
        const Config::Locations& theLocations = theNode.GetLocations();
        for (Config::Locations::const_iterator theIt = theLocations.begin();
                theLocations.end() != theIt;
                ++theIt) {
            const ServerLocation& theLocation = *theIt;
            if (! theLocation.IsValid()) {
                continue;
            }
            List::Iterator theTIt(theTransmittersPtr);
            Transmitter*   theTPtr;
            while ((theTPtr = theTIt.Next())) {
                if (theTPtr->GetId() == theId &&
                        theTPtr->GetLocation() == theLocation) {
                    List::Remove(theTransmittersPtr, *theTPtr);
                    break;
                }
            }
            if (theTPtr) {
                theTPtr->SetActive(
                    0 != (theNode.GetFlags() & Config::kFlagActive));
            } else {
                theTPtr = new Transmitter(*this, theLocation, theId,
                    0 != (theNode.GetFlags() & Config::kFlagActive),
                    theLastLogSeq);
            }
            theTransmittersCount++;
            Insert(*theTPtr);
        }
    }
    Transmitter* theTPtr;
    while ((theTPtr = List::PopFront(theTransmittersPtr))) {
        delete theTPtr;
    }
    mTransmittersCount = theTransmittersCount;
    mNodeId            = inMetaVrSM.GetNodeId();
    mMinAckToCommit    = inMetaVrSM.GetQuorum();
    StartTransmitters(theAuthCtxPtr);
    Update();
    return mTransmittersCount;
}

    void
LogTransmitter::Impl::Suspend(
    bool inFlag)
{
    if (inFlag == mSuspendedFlag) {
        return;
    }
    mSuspendedFlag = inFlag;
    const bool     theStartFlag = ! mSuspendedFlag &&
        mMetaVrSMPtr && mMetaVrSMPtr->IsPrimary();
    List::Iterator theIt(mTransmittersPtr);
    Transmitter*   thePtr;
    while ((thePtr = theIt.Next())) {
        if (mSuspendedFlag) {
            thePtr->Shutdown();
        } else if (theStartFlag) {
            thePtr->Start();
        }
    }
    Update();
}

    void
LogTransmitter::Impl::ScheduleHelloTransmit()
{
    List::Iterator theIt(mTransmittersPtr);
    Transmitter*   thePtr;
    while ((thePtr = theIt.Next())) {
        thePtr->ScheduleHelloTransmit();
    }
}

    void
LogTransmitter::Impl::ScheduleHeartbeatTransmit()
{
    List::Iterator theIt(mTransmittersPtr);
    Transmitter*   thePtr;
    while ((thePtr = theIt.Next())) {
        thePtr->ScheduleHeartbeatTransmit();
    }
}

LogTransmitter::LogTransmitter(
    NetManager&                     inNetManager,
    LogTransmitter::CommitObserver& inCommitObserver)
    : mImpl(*(new Impl(*this, inNetManager, inCommitObserver)))
    {}

LogTransmitter::~LogTransmitter()
{
    delete &mImpl;
}

    void
LogTransmitter::SetFileSystemId(
    int64_t inFsId)
{
    mImpl.SetFileSystemId(inFsId);
}

    int
LogTransmitter::SetParameters(
    const char*       inParamPrefixPtr,
    const Properties& inParameters)
{
    return mImpl.SetParameters(inParamPrefixPtr, inParameters);
}

    int
LogTransmitter::TransmitBlock(
    const MetaVrLogSeq& inBlockSeq,
    int                 inBlockSeqLen,
    const char*         inBlockPtr,
    size_t              inBlockLen,
    uint32_t            inChecksum,
    size_t              inChecksumStartPos)
{
    return mImpl.TransmitBlock(inBlockSeq, inBlockSeqLen,
        inBlockPtr, inBlockLen, inChecksum, inChecksumStartPos);
}

    bool
LogTransmitter::IsUp()
{
    return mImpl.IsUp();
}

    void
LogTransmitter::Suspend(
    bool inFlag)
{
    return mImpl.Suspend(inFlag);
}

    void
LogTransmitter::QueueVrRequest(
    MetaVrRequest&         inVrReq,
    LogTransmitter::NodeId inNodeId)
{
    mImpl.QueueVrRequest(inVrReq, inNodeId);
}

    int
LogTransmitter::Update(
    MetaVrSM& inMetaVrSM)
{
    return mImpl.Update(inMetaVrSM);
}

    void
LogTransmitter::GetStatus(
    LogTransmitter::StatusReporter& inReporter)
{
    return mImpl.GetStatus(inReporter);
}

    void
LogTransmitter::SetHeartbeatInterval(
    int inPrimaryTimeoutSec)
{
    mImpl.SetHeartbeatInterval(inPrimaryTimeoutSec);
    mImpl.ScheduleHeartbeatTransmit();
}

    int
LogTransmitter::GetChannelsCount() const
{
    return mImpl.GetChannelsCount();
}

    void
LogTransmitter::ScheduleHelloTransmit()
{
    mImpl.ScheduleHelloTransmit();
}

} // namespace KFS
