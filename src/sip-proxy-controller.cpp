/*
Copyright (c) 2013, David C Horton

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

namespace drachtio {
    class SipDialogController ;
}

#include <algorithm> // for remove_if
#include <functional> // for unary_function
#
#include <boost/regex.hpp>
#include <boost/bind.hpp>

#include <sofia-sip/sip_util.h>
#include <sofia-sip/msg_header.h>
#include <sofia-sip/msg_addr.h>

#include "sip-proxy-controller.hpp"
#include "controller.hpp"
#include "pending-request-controller.hpp"
#include "cdr.hpp"

#define TIMER_C_MSECS (30 * 1000)
#define TIMER_B_MSECS (NTA_SIP_T1 * 64)
#define TIMER_D_MSECS (32500)

static nta_agent_t* nta = NULL ;
static drachtio::SipProxyController* theProxyController = NULL ;

namespace {
    void cloneProxy(su_root_magic_t* p, su_msg_r msg, void* arg ) {
        drachtio::DrachtioController* pController = reinterpret_cast<drachtio::DrachtioController*>( p ) ;
        drachtio::SipProxyController::ProxyData* d = reinterpret_cast<drachtio::SipProxyController::ProxyData*>( arg ) ;
        pController->getProxyController()->doProxy(d) ;
    }
} ;


namespace drachtio {

    bool ClientTransactionIsTerminated( const boost::shared_ptr<ProxyCore::ClientTransaction> pClient ) {
        return pClient->getTransactionState() == ProxyCore::ClientTransaction::terminated ;
    }
    bool ClientTransactionIsCallingOrProceeding( const boost::shared_ptr<ProxyCore::ClientTransaction> pClient ) {
        return pClient->getTransactionState() == ProxyCore::ClientTransaction::calling ||
            pClient->getTransactionState() == ProxyCore::ClientTransaction::proceeding;
    }
    bool bestResponseOrder( boost::shared_ptr<ProxyCore::ClientTransaction> c1, boost::shared_ptr<ProxyCore::ClientTransaction> c2 ) {
        //prefer a final response over anything else
        if( ProxyCore::ClientTransaction::completed == c1->getTransactionState() && 
            ProxyCore::ClientTransaction::completed != c2->getTransactionState() ) return true ;

        if( ProxyCore::ClientTransaction::completed != c1->getTransactionState() && 
            ProxyCore::ClientTransaction::completed == c2->getTransactionState() ) return false ;

        if( ProxyCore::ClientTransaction::completed == c1->getTransactionState() && 
            ProxyCore::ClientTransaction::completed == c2->getTransactionState()) {
            
            //ordering of final responses as per RFC 3261 16.7.6
            int c1Status = c1->getSipStatus() ;
            int c2Status = c2->getSipStatus()  ;

            if( c1Status >= 600 ) return true ;
            if( c2Status >= 600 ) return false ;

            switch( c1Status ) {
                case 401:
                case 407:
                case 415:
                case 420:
                case 484:
                    return true ;
                default:
                    switch( c2Status ) {
                        case 401:
                        case 407:
                        case 415:
                        case 420:
                        case 484:
                            return false ;
                        default:
                            break ;

                    }
            }
            if( c1Status >= 400 && c1Status <= 499 ) return true ;
            if( c2Status >= 400 && c2Status <= 499 ) return false ;
            if( c1Status >= 500 && c1Status <= 599 ) return true ;
            if( c2Status >= 500 && c2Status <= 599 ) return false ;
        }
        return true ;
    }

    ///ServerTransaction
    ProxyCore::ServerTransaction::ServerTransaction(boost::shared_ptr<ProxyCore> pCore, msg_t* msg) : 
        m_pCore(pCore), m_msg(msg), m_canceled(false), m_sipStatus(0) {

        msg_ref(m_msg) ;
    }
    ProxyCore::ServerTransaction::~ServerTransaction() {
        DR_LOG(log_debug) << "ServerTransaction::~ServerTransaction" ;
        msg_unref(m_msg) ;
    }
    msg_t* ProxyCore::ServerTransaction::msgDup() {
        return msg_dup( m_msg ) ;
    }
    bool ProxyCore::ServerTransaction::isRetransmission(sip_t* sip) {
        return sip->sip_request->rq_method == sip_object( m_msg )->sip_request->rq_method ; //NB: we already know that the Call-Id matches
    }
    bool ProxyCore::ServerTransaction::forwardResponse( msg_t* msg, sip_t* sip ) {
        int rc = nta_msg_tsend( nta, msg_ref(msg), NULL, TAG_END() ) ;
        if( rc < 0 ) {
            DR_LOG(log_error) << "ServerTransaction::forwardResponse failed proxying response " << std::dec << 
                sip->sip_status->st_status << " " << sip->sip_call_id->i_id << ": error " << rc ; 
            msg_unref(msg) ;
            return false ;            
        }
        bool bRetransmitFinal = m_sipStatus >= 200 &&  sip->sip_status->st_status >= 200 ;
        if( !bRetransmitFinal ) m_sipStatus = sip->sip_status->st_status ;

        if( !bRetransmitFinal && sip->sip_cseq->cs_method == sip_method_invite && sip->sip_status->st_status >= 200 ) {
            writeCdr( msg, sip ) ;
        }
        msg_unref(msg) ;
        return true ;
    }
    void ProxyCore::ServerTransaction::writeCdr( msg_t* msg, sip_t* sip ) {
        if( 200 == sip->sip_status->st_status ) {
            Cdr::postCdr( boost::make_shared<CdrStart>( msg, "application", Cdr::proxy_uas ) );                
        }
        else if( sip->sip_status->st_status > 200 ) {
            Cdr::postCdr( boost::make_shared<CdrStop>( msg, "application",
                487 == sip->sip_status->st_status ? Cdr::call_canceled : Cdr::call_rejected ) );
        }        
    }
    bool ProxyCore::ServerTransaction::generateResponse( int status, const char *szReason ) {
       msg_t* reply = nta_msg_create(nta, 0) ;
        msg_ref(reply) ;
        nta_msg_mreply( nta, reply, sip_object(reply), status, szReason, 
            msg_ref(m_msg), //because it will lose a ref in here
            TAG_END() ) ;

        if( sip_method_invite == sip_object(m_msg)->sip_request->rq_method && status >= 200 ) {
            Cdr::postCdr( boost::make_shared<CdrStop>( reply, "application", Cdr::call_rejected ) );
        }

        msg_unref(reply) ;  

        return true ;      
    }


    ///ClientTransaction
    ProxyCore::ClientTransaction::ClientTransaction(boost::shared_ptr<ProxyCore> pCore, const string& target) : 
        m_pCore(pCore), m_target(target), m_canceled(false), m_sipStatus(0),
        m_timerA(NULL), m_timerB(NULL), m_timerC(NULL), m_timerD(NULL), m_msgFinal(NULL),
        m_transmitCount(0), m_method(sip_method_unknown), m_state(not_started) {

        string random ;
        generateUuid( random ) ;
        m_branch = string("z9hG4bK-") + random ;
    }
    ProxyCore::ClientTransaction::~ClientTransaction() {
        DR_LOG(log_debug) << "ClientTransaction::~ClientTransaction" ;
        if( m_timerA ) theProxyController->removeTimer( m_timerA, "timerA" ) ;
        if( m_timerB ) theProxyController->removeTimer( m_timerB, "timerB" ) ;
        if( m_timerC ) theProxyController->removeTimer( m_timerC, "timerC" ) ;
        if( m_timerD ) theProxyController->removeTimer( m_timerD, "timerD" ) ;
        if( m_msgFinal ) msg_unref( m_msgFinal ) ;
    }
    const char* ProxyCore::ClientTransaction::getStateName( State_t state) {
        static const char* szNames[] = {
            "NOT STARTED",
            "CALLING",
            "PROCEEDING",
            "COMPLETED",
            "TERMINATED",
        } ;
        return szNames[ static_cast<int>( state ) ] ;
    }
    void ProxyCore::ClientTransaction::removeTimer( TimerEventHandle& handle, const char* szTimer ) {
        assert(handle) ;
        theProxyController->removeTimer( handle, szTimer ) ;
        handle = NULL ;
    }
    void ProxyCore::ClientTransaction::setState( State_t newState ) {
        if( newState == m_state ) return ;

        boost::shared_ptr<ProxyCore> pCore = m_pCore.lock() ;
        assert( pCore ) ;

        DR_LOG(log_info) << getStateName(m_state) << " --> " << getStateName(newState) << " " << pCore->getCallId();

        m_state = newState ;

        //set transaction state timers
        if( sip_method_invite == m_method ) {
            switch( m_state ) {
                case calling:
                    assert( !m_timerA ) ; //TODO: should only be doing this on unreliable transports
                    assert( !m_timerB ) ;
                    assert( !m_timerC ) ;

                    //timer A = retransmission timer 
                    m_timerA = theProxyController->addTimer("timerA", 
                        boost::bind(&ProxyCore::timerA, pCore, shared_from_this()), NULL, m_durationTimerA = NTA_SIP_T1 ) ;

                    //timer B = timeout when all invite retransmissions have been exhausted
                    m_timerB = theProxyController->addTimer("timerB", 
                        boost::bind(&ProxyCore::timerB, pCore, shared_from_this()), NULL, TIMER_B_MSECS ) ;
                    
                    //timer C - timeout to wait for final response before returning 408 Request Timeout. 
                    m_timerC = theProxyController->addTimer("timerC", 
                        boost::bind(&ProxyCore::timerC, pCore, shared_from_this()), NULL, TIMER_C_MSECS ) ;
                break ;

                case proceeding:
                    removeTimer( m_timerA, "timerA" ) ;
                    removeTimer( m_timerB, "timerB" ) ;
                break; 

                case completed:
                    if( m_timerA ) removeTimer( m_timerA, "timerA" ) ;
                    if( m_timerB ) removeTimer( m_timerB, "timerB" ) ;

                    //timer D - timeout when transaction can move from completed state to terminated
                    assert( !m_timerD ) ;
                    m_timerD = theProxyController->addTimer("timerD", boost::bind(&ProxyCore::timerD, pCore, shared_from_this()), 
                        NULL, TIMER_D_MSECS ) ;
                break ;

                case terminated:
                    if( m_timerA ) removeTimer( m_timerA, "timerA" ) ;
                    if( m_timerB ) removeTimer( m_timerB, "timerB" ) ;
                break ;

                default:
                break; 
            }
        }
    }
    bool ProxyCore::ClientTransaction::matchesResponse( sip_t* sip ) {
        return 0 == m_branch.compare(sip->sip_via->v_branch) && sip->sip_cseq->cs_method == m_method ;
    }
    bool ProxyCore::ClientTransaction::forwardDifferentRequest(msg_t* msg, sip_t* sip) {
        boost::shared_ptr<ProxyCore> pCore = m_pCore.lock() ;
        assert( pCore ) ;

        if( terminated == m_state ) {
            DR_LOG(log_info) << "Not forwarding late-arriving request " << sip->sip_request->rq_method_name <<
                " " << sip->sip_call_id->i_id ;
            nta_msg_discard( nta, msg ) ;
            return true ;
        }
        int rc = nta_msg_tsend( nta, 
            msg, 
            URL_STRING_MAKE(m_target.c_str()), 
            TAG_IF( pCore->shouldAddRecordRoute(), SIPTAG_RECORD_ROUTE(pCore->getMyRecordRoute() ) ),
            NTATAG_BRANCH_KEY(m_branch.c_str()),
            TAG_END() ) ;
        if( rc < 0 ) {
            DR_LOG(log_error) << "forwardDifferentRequest: error forwarding request " ;
            return false ;
        }
        return true ;
    }
    bool ProxyCore::ClientTransaction::forwardRequest() {
        boost::shared_ptr<ProxyCore> pCore = m_pCore.lock() ;
        assert( pCore ) ;

        msg_t* msg = pCore->getServerTransaction()->msgDup() ;
        sip_t* sip = sip_object(msg) ;

        m_transmitCount++ ;

        if( not_started == m_state ) {
            assert( 1 == m_transmitCount ) ;

            string random ;
            generateUuid( random ) ;
            m_branch = string("z9hG4bK-") + random ;
            m_method = sip->sip_request->rq_method ;

            setState( calling ) ;
        }

        //Max-Forwards: decrement or set to 70 
        if( sip->sip_max_forwards ) {
            sip->sip_max_forwards->mf_count-- ;
        }
        else {
            sip_add_tl(msg, sip, SIPTAG_MAX_FORWARDS_STR("70"), TAG_END());
        }

        sip_request_t *rq = sip_request_format(msg_home(msg), "%s %s SIP/2.0", sip->sip_request->rq_method_name, m_target.c_str() ) ;
        msg_header_replace(msg, NULL, (msg_header_t *)sip->sip_request, (msg_header_t *) rq) ;

        tagi_t* tags = makeTags( pCore->getHeaders() ) ;

        int rc = nta_msg_tsend( nta, 
            msg_ref(msg), 
            URL_STRING_MAKE(m_target.c_str()), 
            TAG_IF( pCore->shouldAddRecordRoute(), SIPTAG_RECORD_ROUTE(pCore->getMyRecordRoute() ) ),
            NTATAG_BRANCH_KEY(m_branch.c_str()),
            TAG_NEXT(tags) ) ;

        deleteTags( tags ) ;

        if( rc < 0 ) {
            setState( terminated ) ;
            m_sipStatus = 503 ; //RFC 3261 16.9, but we should validate the request-uri to prevent errors sending to malformed uris
            msg_unref(msg) ;
            return true ;
        }

        if( 1 == m_transmitCount && sip_method_invite == m_method ) {
            Cdr::postCdr( boost::make_shared<CdrAttempt>( msg, "application" ) );
        }

         msg_unref(msg) ;
        
        return true ;
    }
    bool ProxyCore::ClientTransaction::retransmitRequest() {
        m_durationTimerA <<= 1 ;
        DR_LOG(log_debug) << "ClientTransaction - retransmitting request, timer A will be set to " << dec << m_durationTimerA << "ms" ;
        if( forwardRequest() ) {
            boost::shared_ptr<ProxyCore> pCore = m_pCore.lock() ;
            assert( pCore ) ;
            m_timerA = theProxyController->addTimer("timerA", 
                boost::bind(&ProxyCore::timerA, pCore, shared_from_this()), NULL, m_durationTimerA) ;
            return true ;
        }
        return false ;
     }

     bool ProxyCore::ClientTransaction::processResponse( msg_t* msg, sip_t* sip ) {
        bool bForward = false ;

        if( 0 != m_branch.compare(sip->sip_via->v_branch) ) return false ; 

        boost::shared_ptr<ClientTransaction> me = shared_from_this() ;
        boost::shared_ptr<ProxyCore> pCore = m_pCore.lock() ;
        assert( pCore ) ;

        if( terminated == m_state ) {
            DR_LOG(log_info) << "Discarding late-arriving response because transaction is terminated " <<
                sip->sip_status->st_status << " " << sip->sip_cseq->cs_method << " " << sip->sip_call_id->i_id ;
            nta_msg_discard( nta, msg ) ;
            return true ;
        }

        //response to our original request?
        if( m_method == sip->sip_cseq->cs_method ) {    

            //retransmission of final response?
            if( m_sipStatus >= 200 && sip->sip_status->st_status >= 200 ) {
                ackResponse( msg ) ;
                nta_msg_discard( nta, msg ) ;
                return true ;               
            }
            m_sipStatus = sip->sip_status->st_status ;

            //set new state, (re)set timers
            if( m_sipStatus >= 100 && m_sipStatus <= 199 ) {
                setState( proceeding ) ;

                if( 100 != m_sipStatus && sip_method_invite == sip->sip_cseq->cs_method ) {
                    assert( m_timerC ) ;
                    removeTimer( m_timerC, "timerC" ) ;
                    m_timerC = theProxyController->addTimer("timerC", boost::bind(&ProxyCore::timerC, pCore, shared_from_this()), 
                        NULL, TIMER_C_MSECS ) ;
                }                  
            }
            else if( m_sipStatus >= 200 && m_sipStatus <= 299 ) {
                //NB: order is important here - 
                //proxy core will attempt to cancel any invite not in the terminated state
                //so set that first before announcing our success
                setState( terminated ) ;
                pCore->notifyForwarded200OK( me ) ; 
            }
            else if( m_sipStatus >= 300 ) {
                setState( completed ) ;
            }

            if( m_sipStatus >= 200 && sip_method_invite == sip->sip_cseq->cs_method ) {
                if( m_timerC ) removeTimer(m_timerC, "timerC") ;
            }

            //determine whether to forward this response upstream
            if( 100 == m_sipStatus ) {
                DR_LOG(log_debug) << "discarding 100 Trying since we are a stateful proxy" ;
                nta_msg_discard( nta, msg ) ;
                return true ;
            }
            if( m_sipStatus > 100 && m_sipStatus < 300 ) {
                //forward immediately: RFC 3261 16.7.5
                bForward = true ;
            }
            if( m_sipStatus >= 300 ) {
                
                //save final response for later forwarding
                m_msgFinal = msg ;
                msg_ref( m_msgFinal ) ;

                ackResponse( msg ) ;

                if( m_sipStatus >= 300 && m_sipStatus <= 399 && pCore->shouldFollowRedirects() ) {
                    //TODO: put redirects into vector of clients
                }
            }     

            //write CDRS on the UAC side for final response to an INVITE
            if( sip_method_invite == sip->sip_cseq->cs_method && m_sipStatus >= 200 ) {
                writeCdr( msg, sip ) ;
            }       
        }
        else if( sip_method_cancel == sip->sip_cseq->cs_method ) {
            DR_LOG(log_debug) << "Received " << sip->sip_status->st_status << " response to CANCEL" ;
            nta_msg_discard( nta, msg ); 
            return true ;
        }
        else {
            DR_LOG(log_debug) << "Received " << sip->sip_status->st_status << " " << sip->sip_cseq->cs_method_name <<
                " response, forwarding upstream" ;            
        }

        if( bForward ) {
            bool bOK = pCore->getServerTransaction()->forwardResponse( msg, sip ) ;
        }  
        else {
            nta_msg_discard( nta, msg ) ;
        }      
        return true ;
    }
    int ProxyCore::ClientTransaction::cancelRequest() {

        if( proceeding != m_state ) {
            DR_LOG(log_debug) << "cancelRequest - returning without canceling because state is not PROCEEDING it is " << 
                getStateName(m_state) ;
            return 0 ;
        }

        boost::shared_ptr<ProxyCore> pCore = m_pCore.lock() ;
        assert( pCore ) ;

        sip_t* sip = sip_object( pCore->getServerTransaction()->msgDup() );

        msg_t *cmsg = nta_msg_create(nta, 0);
        sip_t *csip = sip_object(cmsg);
        url_string_t const *ruri;

        nta_outgoing_t *cancel = NULL ;
        sip_request_t *rq;
        sip_cseq_t *cseq;
        su_home_t *home = msg_home(cmsg);

        if (csip == NULL)
        return -1;

        sip_add_tl(cmsg, csip,
            SIPTAG_TO(sip->sip_to),
            SIPTAG_FROM(sip->sip_from),
            SIPTAG_CALL_ID(sip->sip_call_id),
            TAG_END());

        if (!(cseq = sip_cseq_create(home, sip->sip_cseq->cs_seq, SIP_METHOD_CANCEL)))
            goto err;
        else
            msg_header_insert(cmsg, (msg_pub_t *)csip, (msg_header_t *)cseq);

        if (!(rq = sip_request_format(home, "CANCEL %s SIP/2.0", m_target.c_str() )))
            goto err;
        else
            msg_header_insert(cmsg, (msg_pub_t *)csip, (msg_header_t *)rq);

        if( nta_msg_tsend( nta, cmsg, NULL, 
            NTATAG_BRANCH_KEY(m_branch.c_str()),
            TAG_END() ) < 0 )
 
            goto err ;

        m_canceled = true ;
        return 0;

        err:
            if( cmsg ) msg_unref(cmsg);
            return -1;
    }
    void ProxyCore::ClientTransaction::writeCdr( msg_t* msg, sip_t* sip ) {
        string encodedMessage ;
        EncodeStackMessage( sip, encodedMessage ) ;
        if( 200 == m_sipStatus ) {
            Cdr::postCdr( boost::make_shared<CdrStart>( msg, "network", Cdr::proxy_uac ), encodedMessage );                
        }               
        else {
            Cdr::postCdr( boost::make_shared<CdrStop>( msg, "network",  
                487 == m_sipStatus ? Cdr::call_canceled : Cdr::call_rejected ), encodedMessage );                
        }        
    }

    ///ProxyCore
    ProxyCore::ProxyCore(const string& clientMsgId, const string& transactionId, tport_t* tp,bool recordRoute, 
        bool fullResponse, const string& headers ) : 
        m_clientMsgId(clientMsgId), m_transactionId(transactionId), m_tp(tp), m_canceled(false), m_headers(headers),
        m_fullResponse(fullResponse), m_bRecordRoute(recordRoute), m_launchType(ProxyCore::serial), m_searching(true) {
    }
    ProxyCore::~ProxyCore() {
        DR_LOG(log_debug) << "ProxyCore::~ProxyCore" ;
    }
    void ProxyCore::initializeTransactions( msg_t* msg, const vector<string>& vecDestination ) {
        m_pServerTransaction = boost::make_shared<ServerTransaction>( shared_from_this(), msg ) ;

        for( vector<string>::const_iterator it = vecDestination.begin(); it != vecDestination.end(); it++ ) {
            boost::shared_ptr<ClientTransaction> pClient = boost::make_shared<ClientTransaction>( shared_from_this(), *it ) ;
            m_vecClientTransactions.push_back( pClient ) ;
        }
        DR_LOG(log_debug) << "initializeTransactions - added " << dec << m_vecClientTransactions.size() << " client transactions " ;
    }
    void ProxyCore::removeTerminated() {
        int nBefore =  m_vecClientTransactions.size() ;
        m_vecClientTransactions.erase(
            std::remove_if( m_vecClientTransactions.begin(), m_vecClientTransactions.end(), ClientTransactionIsTerminated),
            m_vecClientTransactions.end() ) ;
        int nAfter = m_vecClientTransactions.size() ;
        DR_LOG(log_debug) << " removeTerminated - removed " << dec << nBefore-nAfter << ", leaving " << nAfter ;

        if( 0 == nAfter ) {
            DR_LOG(log_debug) << "removeTerminated - all client TUs are terminated, removing proxy core" ;
            theProxyController->removeProxy( shared_from_this() ) ;
        }
    }
    void ProxyCore::notifyForwarded200OK( boost::shared_ptr<ClientTransaction> pClient ) {
        DR_LOG(log_debug) << "forwarded 200 OK terminating other clients" ;
        m_searching = false ;
        cancelOutstandingRequests() ;
    }

    //timer functions
    

    void ProxyCore::timerA(boost::shared_ptr<ClientTransaction> pClient) {
        assert( pClient->getTransactionState() == ClientTransaction::calling ) ;
        pClient->clearTimerA() ;
        pClient->retransmitRequest() ;
    }
    void ProxyCore::timerB(boost::shared_ptr<ClientTransaction> pClient) {
        DR_LOG(log_debug) << "timer B fired for a client transaction" ;
        assert( pClient->getTransactionState() == ClientTransaction::calling ) ;
        pClient->clearTimerB() ;
        pClient->setState( ClientTransaction::terminated ) ;
        removeTerminated() ;
    }
    void ProxyCore::timerC(boost::shared_ptr<ClientTransaction> pClient) {
        DR_LOG(log_debug) << "timer C fired for a client transaction" ;
        assert( pClient->getTransactionState() == ClientTransaction::proceeding || 
             pClient->getTransactionState() == ClientTransaction::calling ) ;
        pClient->clearTimerC() ;
        if( pClient->getTransactionState() == ClientTransaction::proceeding ) {
            pClient->cancelRequest() ;
        }
        m_pServerTransaction->generateResponse(408) ;
        pClient->setState( ClientTransaction::terminated ) ;
        removeTerminated() ;
    }
    void ProxyCore::timerD(boost::shared_ptr<ClientTransaction> pClient) {
        DR_LOG(log_debug) << "timer D fired for a client transaction" ;
        assert( pClient->getTransactionState() == ClientTransaction::completed ) ;
        pClient->clearTimerD() ;
        pClient->setState( ClientTransaction::terminated ) ;
        removeTerminated() ;
    }

    int ProxyCore::startRequests() {
        
        if( !m_searching ) {
            DR_LOG(log_debug) << "startRequests: Proxy is completed so not starting any new requests"; 
            return 0 ;
        }

        int count = 0 ;
        int idx = 0 ;
        vector< boost::shared_ptr<ClientTransaction> >::const_iterator it = m_vecClientTransactions.begin() ;
        for( ; it != m_vecClientTransactions.end(); ++it, idx++ ) {
            DR_LOG(log_debug) << "startRequests: evalating client " << idx ; 
            boost::shared_ptr<ClientTransaction> pClient = *it ;
            if( ClientTransaction::not_started == pClient->getTransactionState() ) {
                DR_LOG(log_debug) << "launching client " << idx ;
                bool sent = pClient->forwardRequest() ;
                if( sent ) count++ ;
                if( sent && ProxyCore::serial == getLaunchType() ) {
                    break ;
                }
            }
        }
        DR_LOG(log_debug) << "startRequests: started " << dec << count << " clients"; 
        return count ;
    }
    void ProxyCore::cancelOutstandingRequests() {
        m_searching = false ;
        vector< boost::shared_ptr<ProxyCore::ClientTransaction> >::const_iterator it = m_vecClientTransactions.begin() ;
        for( ; it != m_vecClientTransactions.end(); ++it ) {
            boost::shared_ptr<ProxyCore::ClientTransaction> pClient = *it ;
            pClient->cancelRequest() ;
        }        
    }

    const string& ProxyCore::getTransactionId() { return m_transactionId; }
    tport_t* ProxyCore::getTport() { return m_tp; }
    /*
    void ProxyCore::setProvisionalTimeout(const string& t ) {
        boost::regex e("^(\\d+)(ms|s)$", boost::regex::extended);
        boost::smatch mr;
        if( boost::regex_search( t, mr, e ) ) {
            string s = mr[1] ;
            m_provisionalTimeout = ::atoi( s.c_str() ) ;
            if( 0 == mr[2].compare("s") ) {
                m_provisionalTimeout *= 1000 ;
            }
            DR_LOG(log_debug) << "provisional timeout is " << m_provisionalTimeout << "ms" ;
        }
        else if( t.length() > 0 ) {
            DR_LOG(log_error) << "Invalid timeout syntax: " << t ;
        }
    }
    void ProxyCore::setFinalTimeout(const string& t ) {
        boost::regex e("^(\\d+)(ms|s)$", boost::regex::extended);
        boost::smatch mr;
        if( boost::regex_search( t, mr, e ) ) {
            string s = mr[1] ;
            m_finalTimeout = ::atoi( s.c_str() ) ;
            if( 0 == mr[2].compare("s") ) {
                m_finalTimeout *= 1000 ;
            }
            DR_LOG(log_debug) << "final timeout is " << m_finalTimeout << "ms" ;
        }
        else if( t.length() > 0 ) {
            DR_LOG(log_error) << "Invalid timeout syntax: " << t ;
        }
    }
    */
    const sip_record_route_t* ProxyCore::getMyRecordRoute(void) {
        return theOneAndOnlyController->getMyRecordRoute() ;
    }
    bool ProxyCore::allClientsAreTerminated(void) {
        vector< boost::shared_ptr<ProxyCore::ClientTransaction> >::const_iterator it = m_vecClientTransactions.begin() ;
        for(; it != m_vecClientTransactions.end(); ++it ) {
            boost::shared_ptr<ProxyCore::ClientTransaction> pClient = *it ;
            if( ClientTransaction::completed != pClient->getTransactionState() && 
                ClientTransaction::terminated != pClient->getTransactionState() ) return false ;
        }
        return true ;
    }
    bool ProxyCore::processResponse(msg_t* msg, sip_t* sip) {
        bool handled = false ;
        vector< boost::shared_ptr<ClientTransaction> >::const_iterator it = m_vecClientTransactions.begin() ;
        for( ; it != m_vecClientTransactions.end() && !handled; ++it ) {
            boost::shared_ptr<ClientTransaction> pClient = *it ;
            if( pClient->processResponse( msg, sip ) ) {
                handled = true ;
            }
        }
        removeTerminated() ;

        if( m_searching && exhaustedAllTargets() ) {
            forwardBestResponse() ;
        }
        return handled ;
    }
    bool ProxyCore::exhaustedAllTargets() {
        return  m_vecClientTransactions.end() == std::find_if( m_vecClientTransactions.begin(), m_vecClientTransactions.end(), 
            ClientTransactionIsCallingOrProceeding ) ;
    }
    void ProxyCore::forwardBestResponse() {
        m_searching = false ;
        std::sort( m_vecClientTransactions.begin(), m_vecClientTransactions.end(), bestResponseOrder ) ;
        if( 0 == m_vecClientTransactions.size() || ClientTransaction::completed != m_vecClientTransactions.at(0)->getTransactionState() ) {
            DR_LOG(log_debug) << "forwardBestResponse - sending 408 as there are no canidate final responses"  ;
            m_pServerTransaction->generateResponse( 408 ) ;
        }
        else {
            msg_t* msg = m_vecClientTransactions.at(0)->getFinalResponse() ;
            assert( msg ) ;

            DR_LOG(log_debug) << "forwardBestResponse - selected " << m_vecClientTransactions.at(0)->getSipStatus() << 
                " as best non-success status to return"  ;

            msg_ref(msg); 
            m_pServerTransaction->forwardResponse( msg, sip_object(msg) ) ;      
        }
    }

    SipProxyController::SipProxyController( DrachtioController* pController, su_clone_r* pClone ) : m_pController(pController), m_pClone(pClone), 
        m_agent(pController->getAgent()), m_queue(pController->getRoot()), m_queueB(pController->getRoot(),"timerB"),
        m_queueC(pController->getRoot(),"timerC"), m_queueD(pController->getRoot(),"timerD")   {

            assert(m_agent) ;
            nta = m_agent ;
            theProxyController = this ;

    }
    SipProxyController::~SipProxyController() {
    }

    void SipProxyController::proxyRequest( const string& clientMsgId, const string& transactionId, bool recordRoute, 
        bool fullResponse, bool followRedirects, const string& provisionalTimeout, const string& finalTimeout, 
        const vector<string>& vecDestinations, const string& headers )  {

        DR_LOG(log_debug) << "SipProxyController::proxyRequest - transactionId: " << transactionId ;
        boost::shared_ptr<PendingRequest_t> p = m_pController->getPendingRequestController()->findAndRemove( transactionId ) ;
        if( !p) {
            string failMsg = "Unknown transaction id: " + transactionId ;
            DR_LOG(log_error) << "SipProxyController::proxyRequest - " << failMsg;  
            m_pController->getClientController()->route_api_response( clientMsgId, "NOK", failMsg) ;
            return ;
        }
        else {
            addProxy( clientMsgId, transactionId, p->getMsg(), p->getSipObject(), p->getTport(), recordRoute, fullResponse, followRedirects, 
                provisionalTimeout, finalTimeout, vecDestinations, headers ) ;
        }

        su_msg_r m = SU_MSG_R_INIT ;
        int rv = su_msg_create( m, su_clone_task(*m_pClone), su_root_task(m_pController->getRoot()),  cloneProxy, sizeof( SipProxyController::ProxyData ) );
        if( rv < 0 ) {
            m_pController->getClientController()->route_api_response( clientMsgId, "NOK", "Internal server error allocating message") ;
            return  ;
        }
        void* place = su_msg_data( m ) ;

        /* we need to use placement new to allocate the object in a specific address, hence we are responsible for deleting it (below) */
        ProxyData* msgData = new(place) ProxyData( clientMsgId, transactionId ) ;
        rv = su_msg_send(m);  
        if( rv < 0 ) {
            m_pController->getClientController()->route_api_response( clientMsgId, "NOK", "Internal server error sending message") ;
            return  ;
        }
        
        return  ;
    } 
    void SipProxyController::doProxy( ProxyData* pData ) {
        string transactionId = pData->getTransactionId()  ;
        DR_LOG(log_debug) << "SipProxyController::doProxy - transactionId: " <<transactionId ;

        boost::shared_ptr<ProxyCore> p = getProxyByTransactionId( transactionId ) ;
        if( !p ) {
            m_pController->getClientController()->route_api_response( pData->getClientMsgId(), "NOK", "transaction id no longer exists") ;
        }
        else {

            msg_t* msg = p->getServerTransaction()->msg() ; ;
            sip_t* sip = sip_object(msg);

            if( sip->sip_max_forwards && sip->sip_max_forwards->mf_count <= 0 ) {
                DR_LOG(log_error) << "SipProxyController::doProxy rejecting request due to max forwards used up " << sip->sip_call_id->i_id ;

                msg_t* reply = nta_msg_create(nta, 0) ;
                msg_ref(reply) ;
                nta_msg_mreply( nta, reply, sip_object(reply), SIP_483_TOO_MANY_HOPS, 
                    msg_ref(p->getServerTransaction()->msg()), //because it will lose a ref in here
                    TAG_END() ) ;

                Cdr::postCdr( boost::make_shared<CdrStop>( reply, "application", Cdr::call_rejected ) );

                msg_unref(reply) ;

                removeProxyByTransactionId( transactionId )  ;
                m_pController->getClientController()->route_api_response( pData->getClientMsgId(), "NOK", 
                    "Rejected with 483 Too Many Hops due to Max-Forwards value of 0" ) ;
                pData->~ProxyData() ; 
                return ;
            }

            //stateful proxy sends 100 Trying
            nta_msg_treply( m_agent, msg_dup(msg), 100, NULL, TAG_END() ) ;                
 
            int clients = p->startRequests() ;

            //check to make sure we got at least one request out
            if( 0 == clients ) {
                m_pController->getClientController()->route_api_response( pData->getClientMsgId(), "NOK", "error proxying request to " ) ;
                DR_LOG(log_error) << "Error proxying request; please check that this is a valid SIP Request-URI and retry" ;

                msg_t* reply = nta_msg_create(nta, 0) ;
                msg_ref(reply) ;
                nta_msg_mreply( nta, reply, sip_object(reply), 500, NULL, 
                    msg_ref(p->getServerTransaction()->msg()), //because it will lose a ref in here
                    TAG_END() ) ;

                Cdr::postCdr( boost::make_shared<CdrStop>( reply, "application", Cdr::call_rejected ) );

                msg_unref(reply) ;

                removeProxyByTransactionId( transactionId ) ;
             }
            else if( !p->wantsFullResponse() ) {
                m_pController->getClientController()->route_api_response( p->getClientMsgId(), "OK", "done" ) ;
            }
        }

        //N.B.: we must explicitly call the destructor of an object allocated with placement new
        pData->~ProxyData() ; 

    }
    bool SipProxyController::processResponse( msg_t* msg, sip_t* sip ) {
        string callId = sip->sip_call_id->i_id ;
        DR_LOG(log_debug) << "SipProxyController::processResponse " << std::dec << sip->sip_status->st_status << " " << callId ;

        boost::shared_ptr<ProxyCore> p = getProxyByCallId( sip->sip_call_id->i_id ) ;

        if( !p ) return false ;

        //search for a matching client transaction to handle the response
        if( !p->processResponse( msg, sip ) ) {
            DR_LOG(log_debug)<< "processResponse - forwarding upstream (not handled by client transactions)" << callId ;
            nta_msg_tsend( nta, msg, NULL, TAG_END() ) ;  
            return true ;          
        }

        //we may be able to start some new requests now
        int countStarted = 0 ;
        if( sip->sip_status->st_status > 200 ) countStarted = p->startRequests() ;

        //check if we are done
        if( 0 == countStarted ) { // && all clients terminated

        }

/*
        if( sip->sip_cseq->cs_method == sip_method_invite ) {
            boost::shared_ptr<ProxyCore> p = getProxyByCallId( sip->sip_call_id->i_id ) ;
            if( !p ) {
                DR_LOG(log_error) << "SipProxyController::processResponse unknown call-id for response " <<  std::dec << sip->sip_status->st_status << 
                    " " << sip->sip_call_id->i_id ;
                return false ;
            }
            int status = sip->sip_status->st_status ;
           
            bool locallyCanceled = p->isCanceledBranch( sip->sip_via->v_branch ) ;
            if( locallyCanceled ) {
                DR_LOG(log_debug) << "Received final response to an INVITE that we canceled, generate ack" ;
                assert( 487 == status ) ;
                ackResponse( msg ) ;

                Cdr::postCdr( boost::make_shared<CdrStop>( msg, "network", Cdr::call_canceled ) );                

                msg_unref( msg ) ;

                return true ;
            }

            p->setLastStatus( status ) ;

            //clear timers, as appropriate 
            clearTimerProvisional(p) ;
            if( status >= 200 ) {
                clearTimerFinal(p) ;
            }

            bool crankback = status > 200 && !isTerminatingResponse( status ) && p->hasMoreTargets() && !p->isCanceled() ;

            //send response back to client
            string encodedMessage ;
            if( p->wantsFullResponse() ) {
                EncodeStackMessage( sip, encodedMessage ) ;
                SipMsgData_t meta(msg) ;
                string s ;
                meta.toMessageFormat(s) ;

                string data = s + "|||continue" + CRLF + encodedMessage ; //no transaction id or dialog id

                m_pController->getClientController()->route_api_response( p->getClientMsgId(), "OK", data ) ;   

                if( status >= 200 && !crankback ) {
                    m_pController->getClientController()->route_api_response( p->getClientMsgId(), "OK", "done" ) ;
                 }             
            }
            if( 200 == status ) removeProxyByCallId( callId ) ;
            
            if( p->isStateful() && 100 == status ) {
                msg_unref( msg ) ;
                return true ;  //in stateful mode we've already sent a 100 Trying  
            }

            //follow a redirect response if we are configured to do so
            if( status >= 300 && status < 399 && p->shouldFollowRedirects() && sip->sip_contact ) {
                sip_contact_t* contact = sip->sip_contact ;
                int i = 0 ;
                vector<string>& vec = p->getDestinations() ;
                for (sip_contact_t* m = sip->sip_contact; m; m = m->m_next, i++) {
                    char buffer[URL_MAXLEN] = "" ;
                    url_e(buffer, URL_MAXLEN, m->m_url) ;

                    DR_LOG(log_debug) << "SipProxyController::processResponse -- adding contact from redirect response " << buffer ;
                    vec.insert( vec.begin() + p->getCurrentOffset() + 1 + i, buffer ) ;
                }
                crankback = true ;
            }

            //write cdrs on the UAC side
            if( 200 == status ) {
                Cdr::postCdr( boost::make_shared<CdrStart>( msg, "network", Cdr::proxy_uac ), encodedMessage );                
            }
            else if( status > 200 ) {
                Cdr::postCdr( boost::make_shared<CdrStop>( msg, "network",  
                    487 == status ? Cdr::call_canceled : Cdr::call_rejected ), encodedMessage );
            }

            //don't send back to client if we are going to fork a new INVITE
            if( crankback ) {
                ackResponse( msg ) ;

                DR_LOG(log_info) << "SipProxyController::processRequestWithoutRouteHeader - proxy crankback to attempt next destination " ;
                proxyToTarget( p, p->getNextTarget() ) ;

                msg_unref( msg ) ;

                return true ;
             }  
        }
        else if( sip->sip_cseq->cs_method == sip_method_cancel ) {
            boost::shared_ptr<ProxyCore> p = getProxyByCallId( sip->sip_call_id->i_id ) ;
            if( p && p->isCanceledBranch( sip->sip_via->v_branch ) ) {
                DR_LOG(log_debug) << "Received response to our CANCEL, discarding" ;
                msg_unref(msg) ;
                return true ;
            }
        }
   
        int rc = nta_msg_tsend( m_pController->getAgent(), 
            msg_ref(msg), 
            NULL, 
            TAG_END() ) ;
        if( rc < 0 ) {
            DR_LOG(log_error) << "SipProxyController::processResponse failed proxying response " << std::dec << sip->sip_status->st_status << 
                " " << sip->sip_call_id->i_id << ": error " << rc ; 
            msg_unref(msg) ;
            return false ;            
        }
        if( sip->sip_cseq->cs_method == sip_method_invite && sip->sip_status->st_status >= 200 ) {
            //write cdrs on the UAS side
            if( 200 == sip->sip_status->st_status ) {
                Cdr::postCdr( boost::make_shared<CdrStart>( msg, "application", Cdr::proxy_uas ) );                
            }
            else if( sip->sip_status->st_status > 200 ) {
                Cdr::postCdr( boost::make_shared<CdrStop>( msg, "application",
                    487 == sip->sip_status->st_status ? Cdr::call_canceled : Cdr::call_rejected ) );
            }
        }
        msg_unref(msg) ;
*/
        return true ;
    }
    bool SipProxyController::processRequestWithRouteHeader( msg_t* msg, sip_t* sip ) {
        string callId = sip->sip_call_id->i_id ;
        string transactionId ;

        DR_LOG(log_debug) << "SipProxyController::processRequestWithRouteHeader " << callId ;

        sip_route_remove( msg, sip) ;

        //generate cdrs on BYE
        if( sip_method_bye == sip->sip_request->rq_method ) {

            Cdr::postCdr( boost::make_shared<CdrStop>( msg, "network", Cdr::normal_release ) );
        }

        int rc = nta_msg_tsend( nta, msg_ref(msg), NULL, TAG_END() ) ;
        if( rc < 0 ) {
            msg_unref(msg) ;
            DR_LOG(log_error) << "SipProxyController::processRequestWithRouteHeader failed proxying request " << callId << ": error " << rc ; 
            return false ;
        }

        if( sip_method_bye == sip->sip_request->rq_method ) {
            Cdr::postCdr( boost::make_shared<CdrStop>( msg, "application", Cdr::normal_release ) );            
        }

        msg_unref(msg) ;

        return true ;
    }
    bool SipProxyController::processRequestWithoutRouteHeader( msg_t* msg, sip_t* sip ) {
        string callId = sip->sip_call_id->i_id ;

        boost::shared_ptr<ProxyCore> p = getProxyByCallId( sip->sip_call_id->i_id ) ;
        if( !p ) {
            DR_LOG(log_error) << "SipProxyController::processRequestWithoutRouteHeader unknown call-id for " <<  
                sip->sip_request->rq_method_name << " " << sip->sip_call_id->i_id ;
            nta_msg_discard( nta, msg ) ;
            return false ;
        }

        if( sip_method_ack == sip->sip_request->rq_method ) {
            //TODO: this is wrong:
            //1. We may get ACKs for success if we Record-Route'd
            //2. What about PRACK ?
            DR_LOG(log_error) << "SipProxyController::processRequestWithoutRouteHeader discarding ACK for non-success response " <<  
                sip->sip_call_id->i_id ;
            nta_msg_discard( nta, msg ) ;
            return true ;
        }

        bool bRetransmission = p->getServerTransaction()->isRetransmission( sip ) ;

        if( bRetransmission ) {
            DR_LOG(log_debug) << "Discarding retransmitted message since we are a stateful proxy" ;
            nta_msg_discard( nta, msg ) ;
            return false ;
        }

        //I think we only expect a CANCEL to come through here
        assert( sip_method_cancel == sip->sip_request->rq_method ) ;

        nta_msg_treply( nta, msg, 200, NULL, TAG_END() ) ;  //200 OK to the CANCEL
        p->getServerTransaction()->generateResponse( 487 ) ;   //487 to INVITE

        p->cancelOutstandingRequests() ;
    
        return true ;
    }
    bool SipProxyController::isProxyingRequest( msg_t* msg, sip_t* sip )  {
      boost::lock_guard<boost::mutex> lock(m_mutex) ;
      mapCallId2Proxy::iterator it = m_mapCallId2Proxy.find( sip->sip_call_id->i_id ) ;
      return it != m_mapCallId2Proxy.end() ;
    }

    boost::shared_ptr<ProxyCore> SipProxyController::removeProxyByTransactionId( const string& transactionId ) {
      boost::shared_ptr<ProxyCore> p ;
      boost::lock_guard<boost::mutex> lock(m_mutex) ;
      mapTxnId2Proxy::iterator it = m_mapTxnId2Proxy.find( transactionId ) ;
      if( it != m_mapTxnId2Proxy.end() ) {
        p = it->second ;
        m_mapTxnId2Proxy.erase(it) ;
        mapCallId2Proxy::iterator it2 = m_mapCallId2Proxy.find( sip_object( p->getServerTransaction()->msg() )->sip_call_id->i_id ) ;
        assert( it2 != m_mapCallId2Proxy.end()) ;
        m_mapCallId2Proxy.erase( it2 ) ;
      }
      assert( m_mapTxnId2Proxy.size() == m_mapCallId2Proxy.size() );
      DR_LOG(log_debug) << "SipProxyController::removeProxyByTransactionId - there are now " << m_mapTxnId2Proxy.size() << " proxy instances" ;
      return p ;
    }
    boost::shared_ptr<ProxyCore> SipProxyController::removeProxyByCallId( const string& callId ) {
      boost::shared_ptr<ProxyCore> p ;
      boost::lock_guard<boost::mutex> lock(m_mutex) ;
      mapCallId2Proxy::iterator it = m_mapCallId2Proxy.find( callId ) ;
      if( it != m_mapCallId2Proxy.end() ) {
        p = it->second ;
        m_mapCallId2Proxy.erase(it) ;
        mapTxnId2Proxy::iterator it2 = m_mapTxnId2Proxy.find( p->getTransactionId() ) ;
        assert( it2 != m_mapTxnId2Proxy.end()) ;
        m_mapTxnId2Proxy.erase( it2 ) ;
      }
      assert( m_mapTxnId2Proxy.size() == m_mapCallId2Proxy.size() );
      DR_LOG(log_debug) << "SipProxyController::removeProxyByTransactionId - there are now " << m_mapCallId2Proxy.size() << " proxy instances" ;
      return p ;
    }
    void SipProxyController::removeProxy( boost::shared_ptr<ProxyCore> pCore ) {
        removeProxyByCallId( sip_object( pCore->getServerTransaction()->msg() )->sip_call_id->i_id ) ;
    }

    bool SipProxyController::isTerminatingResponse( int status ) {
        switch( status ) {
            case 200:
            case 486:
            case 603:
                return true ;
            default:
                return false ;
        }
    }
    TimerEventHandle SipProxyController::addTimer( const char* szTimerClass, TimerFunc f, void* functionArgs, uint32_t milliseconds ) {
        TimerEventHandle handle ;
        if( 0 == strcmp("timerB", szTimerClass) ) handle = m_queueB.add( f, functionArgs, milliseconds );
        else if( 0 == strcmp("timerC", szTimerClass) ) handle = m_queueC.add( f, functionArgs, milliseconds );
        else if( 0 == strcmp("timerD", szTimerClass) ) handle = m_queueD.add( f, functionArgs, milliseconds );
        else handle = m_queue.add( f, functionArgs, milliseconds );
        return handle ;
    }
    void SipProxyController::removeTimer( TimerEventHandle handle, const char* szTimerClass ) {
        if( 0 == strcmp("timerB", szTimerClass) ) m_queueB.remove( handle ) ;
        else if( 0 == strcmp("timerC", szTimerClass) ) m_queueC.remove( handle ) ;
        else if( 0 == strcmp("timerD", szTimerClass) ) m_queueD.remove( handle ) ;
        else m_queue.remove( handle ) ;
    }

    boost::shared_ptr<ProxyCore>  SipProxyController::addProxy( const string& clientMsgId, const string& transactionId, 
        msg_t* msg, sip_t* sip, tport_t* tp, bool recordRoute, bool fullResponse, bool followRedirects,
        const string& provisionalTimeout, const string& finalTimeout, vector<string> vecDestination, const string& headers ) {

      boost::shared_ptr<ProxyCore> p = boost::make_shared<ProxyCore>( clientMsgId, transactionId, tp, recordRoute, 
        fullResponse, headers ) ;
      p->shouldFollowRedirects( followRedirects ) ;
      p->initializeTransactions( msg, vecDestination ) ;
      
      boost::lock_guard<boost::mutex> lock(m_mutex) ;
      m_mapCallId2Proxy.insert( mapCallId2Proxy::value_type(sip->sip_call_id->i_id, p) ) ;
      m_mapTxnId2Proxy.insert( mapTxnId2Proxy::value_type(p->getTransactionId(), p) ) ;   
      return p ;         
    }

    void SipProxyController::logStorageCount(void)  {
        boost::lock_guard<boost::mutex> lock(m_mutex) ;

        DR_LOG(log_debug) << "SipProxyController storage counts"  ;
        DR_LOG(log_debug) << "----------------------------------"  ;
        DR_LOG(log_debug) << "m_mapCallId2Proxy size:                                          " << m_mapCallId2Proxy.size()  ;
        DR_LOG(log_debug) << "m_mapTxnId2Proxy size:                                           " << m_mapTxnId2Proxy.size()  ;
        DR_LOG(log_debug) << "queue size:                                                      " << m_queue.size() ;
        DR_LOG(log_debug) << "timer B queue size:                                              " << m_queueB.size() ;
        DR_LOG(log_debug) << "timer C queue size:                                              " << m_queueC.size() ;
        DR_LOG(log_debug) << "timer D queue size:                                              " << m_queueD.size() ;
    }


} ;
