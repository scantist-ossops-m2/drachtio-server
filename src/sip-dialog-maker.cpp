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
#include <boost/assign/list_of.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/replace.hpp>

#include "sip-dialog-maker.hpp"
#include "controller.hpp"


namespace {
    void cloneRespondToSipRequest(su_root_magic_t* p, su_msg_r msg, void* arg ) {
    	drachtio::DrachtioController* pController = reinterpret_cast<drachtio::DrachtioController*>( p ) ;
        drachtio::SipDialogMaker::InviteResponseData* d = reinterpret_cast<drachtio::SipDialogMaker::InviteResponseData*>( arg ) ;
        pController->getDialogMaker()->doRespondToSipRequest( d ) ;
    }
}


namespace drachtio {

	/* headers that are allowed to be set by the client in responses to sip requests */
	SipDialogMaker::mapHdr2Tag SipDialogMaker::m_mapHdr2Tag = boost::assign::map_list_of
		( string("user_agent"), siptag_user_agent_str ) 
        ( string("subject"), siptag_subject_str ) 
        ( string("max_forwards"), siptag_max_forwards_str ) 
        ( string("proxy_require"), siptag_proxy_require_str ) 
        ( string("request_disposition"), siptag_request_disposition_str ) 
        ( string("accept_contact"), siptag_accept_contact_str ) 
        ( string("reject_contact"), siptag_reject_contact_str ) 
        ( string("expires"), siptag_expires_str ) 
        ( string("date"), siptag_date_str ) 
        ( string("retry_after"), siptag_retry_after_str ) 
        ( string("timestamp"), siptag_timestamp_str ) 
        ( string("min_expires"), siptag_min_expires_str ) 
        ( string("priority"), siptag_priority_str ) 
        ( string("call_info"), siptag_call_info_str ) 
        ( string("organization"), siptag_organization_str ) 
        ( string("server"), siptag_server_str ) 
        ( string("in_reply_to"), siptag_in_reply_to_str ) 
        ( string("accept"), siptag_accept_str ) 
        ( string("accept_encoding"), siptag_accept_encoding_str ) 
        ( string("accept_language"), siptag_accept_language_str ) 
        ( string("allow"), siptag_allow_str ) 
        ( string("require"), siptag_require_str ) 
        ( string("supported"), siptag_supported_str ) 
        ( string("unsupported"), siptag_unsupported_str ) 
        ( string("event"), siptag_event_str ) 
        ( string("allow_events"), siptag_allow_events_str ) 
        ( string("subscription_state"), siptag_subscription_state_str ) 
        ( string("proxy_authenticate"), siptag_proxy_authenticate_str ) 
        ( string("proxy_authentication_info"), siptag_proxy_authentication_info_str ) 
        ( string("proxy_authorization"), siptag_proxy_authorization_str ) 
        ( string("authorization"), siptag_authorization_str ) 
        ( string("www_authenticate"), siptag_www_authenticate_str ) 
        ( string("authentication_info"), siptag_authentication_info_str ) 
        ( string("error_info"), siptag_error_info_str ) 
        ( string("warning"), siptag_warning_str ) 
        ( string("refer_to"), siptag_refer_to_str ) 
        ( string("referred_by"), siptag_referred_by_str ) 
        ( string("replaces"), siptag_replaces_str ) 
        ( string("session_expires"), siptag_session_expires_str ) 
        ( string("min_se"), siptag_min_se_str ) 
        ( string("path"), siptag_path_str ) 
        ( string("service_route"), siptag_service_route_str ) 
        ( string("reason"), siptag_reason_str ) 
        ( string("security_client"), siptag_security_client_str ) 
        ( string("security_server"), siptag_security_server_str ) 
        ( string("security_verify"), siptag_security_verify_str ) 
        ( string("privacy"), siptag_privacy_str ) 
        ( string("etag"), siptag_etag_str ) 
        ( string("if_match"), siptag_if_match_str ) 
        ( string("mime_version"), siptag_mime_version_str ) 
        ( string("content_type"), siptag_content_type_str ) 
        ( string("content_encoding"), siptag_content_encoding_str ) 
        ( string("content_language"), siptag_content_language_str ) 
        ( string("content_disposition"), siptag_content_disposition_str ) 
        ( string("error"), siptag_error_str ) 
		;

	/* headers that are not allowed to be set by the client in responses to sip requests */
	SipDialogMaker::setHdr SipDialogMaker::m_setImmutableHdrs = boost::assign::list_of
		( string("from") ) 
		( string("to") ) 
		( string("call_id") ) 
		( string("cseq") ) 
        ( string("via") ) 
        ( string("route") ) 
        ( string("contact") ) 
        ( string("rseq") ) 
        ( string("rack") ) 
        ( string("record_route") ) 
        ( string("content_length") ) 
        ( string("payload") ) 
		;


	SipDialogMaker::SipDialogMaker( DrachtioController* pController, su_clone_r* pClone ) : m_pController(pController), m_pClone(pClone) {
		map<string,string> t = boost::assign::map_list_of
				( string("Via"), string("") ) 
				;
	}
	SipDialogMaker::~SipDialogMaker() {

	}

    void SipDialogMaker::respondToSipRequest( const string& msgId, boost::shared_ptr<JsonMsg> pMsg  ) {
       DR_LOG(log_debug) << "answerInvite thread id: " << boost::this_thread::get_id() << endl ;

        su_msg_r msg = SU_MSG_R_INIT ;
        int rv = su_msg_create( msg, su_clone_task(*m_pClone), su_root_task(m_pController->getRoot()),  cloneRespondToSipRequest, 
        	sizeof( SipDialogMaker::InviteResponseData ) );
        if( rv < 0 ) {
            return  ;
        }
        void* place = su_msg_data( msg ) ;

        /* we need to use placement new to allocate the object in a specific address, hence we are responsible for deleting it (below) */
        InviteResponseData* msgData = new(place) InviteResponseData( msgId, pMsg ) ;
        rv = su_msg_send(msg);  
        if( rv < 0 ) {
            return  ;
        }
    }
    void SipDialogMaker::doRespondToSipRequest( InviteResponseData* pData ) {
        string msgId( pData->getMsgId() ) ;
        boost::shared_ptr<JsonMsg> pMsg = pData->getMsg() ;

        DR_LOG(log_debug) << "responding to invite in thread " << boost::this_thread::get_id() << " with msgId " << msgId << endl ;

        mapMsgId2IIP::iterator it = m_mapMsgId2IIP.find( msgId ) ;
        if( m_mapMsgId2IIP.end() != it ) {
            boost::shared_ptr<IIP> iip = it->second ;
            nta_leg_t* leg = iip->leg() ;
            nta_incoming_t* irq = iip->irq() ;

            int code ;
            string status ;
            pMsg->get<int>("data.code", code ) ;
            pMsg->get<string>("data.status", status);

            /* iterate through data.opts.headers, adding headers to the response */
            json_spirit::mObject obj ;
            if( pMsg->get<json_spirit::mObject>("data.opts.headers", obj) ) {
                vector<string> vecUnknownStr ;
            	int nHdrs = obj.size() ;
            	tagi_t *tags = new tagi_t[nHdrs+1] ;
            	int i = 0; 
            	for( json_spirit::mConfig::Object_type::iterator it = obj.begin() ; it != obj.end(); it++, i++ ) {

            		/* default to skip, as this may not be a header we are allowed to set, or value might not be provided correctly (as a string) */
					tags[i].t_tag = tag_skip ;
					tags[i].t_value = (tag_value_t) 0 ;            			

            		string hdr = boost::to_lower_copy( boost::replace_all_copy( it->first, "-", "_" ) );

                    /* check to make sure this isn't a header that is not client-editable */
                    if( SipDialogMaker::m_setImmutableHdrs.end() != SipDialogMaker::m_setImmutableHdrs.find( hdr ) ) {
                        DR_LOG(log_error) << "Error: client provided a value for header '" << it->first << "' which is not modfiable by client" << endl;
                        continue ;                       
                    }

                    try {
                        string value = it->second.get_str() ;
                        DR_LOG(log_debug) << "Adding header '" << hdr << "' with value '" << value << "'" << endl ;

                        mapHdr2Tag::const_iterator itTag = SipDialogMaker::m_mapHdr2Tag.find( hdr ) ;
                        if( itTag != SipDialogMaker::m_mapHdr2Tag.end() ) {
                            /* known header */
                            tags[i].t_tag = itTag->second ;
                            tags[i].t_value = (tag_value_t) value.c_str() ;

                        }
                        else {
                            /* custom header */

                            if( string::npos != it->first.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890-_") ) {
                               DR_LOG(log_error) << "Error: client supplied invalid custom header name '" << it->first << "'" << endl;
                               continue ;
                            }
                            else if( string::npos != value.find("\r\n") ) {
                              DR_LOG(log_error) << "Error: client supplied invalid custom header value (contains CR or LF) for header '" << it->first << "'" << endl;
                               continue ;
                            }
                            ostringstream o ;
                            o << it->first << ": " <<  value.c_str()  ;
                            vecUnknownStr.push_back( o.str() ) ;
                            tags[i].t_tag = siptag_unknown_str ;
                            tags[i].t_value = (tag_value_t) vecUnknownStr.back().c_str() ;
                        }
                    } catch( std::runtime_error& err ) {
                        DR_LOG(log_error) << "Error attempting to read string value for header " << hdr << ": " << err.what() << endl;
                    }                       
            	}

            	tags[nHdrs].t_tag = tag_null ;
				tags[nHdrs].t_value = (tag_value_t) 0 ;            	

	            nta_incoming_treply( irq, code, status.empty() ? NULL : status.c_str(), TAG_NEXT(tags) ) ;           	

            	delete[] tags ;
            }
            else {
	            nta_incoming_treply( irq, code, status.empty() ? NULL : status.c_str(), TAG_END() ) ;           	
            }

 
            if( code >= 200 ) {
                m_mapMsgId2IIP.erase( it ) ;
            }
         }
        else {
            DR_LOG(log_warning) << "Unable to find invite-in-progress with msgId " << msgId << endl ;
        }

        /* we must explicitly delete an object allocated with placement new */
        pData->~InviteResponseData() ;
    }
	void SipDialogMaker::addIncomingInviteTransaction( nta_leg_t* leg, nta_incoming_t* irq, const string& msgId ) {
	    boost::shared_ptr<IIP> p = boost::make_shared<IIP>(leg, irq, msgId) ;
	    m_mapIrq2IIP.insert( mapIrq2IIP::value_type(irq, p) ) ;
	    m_mapMsgId2IIP.insert( mapMsgId2IIP::value_type(msgId, p) ) ;		
	}




}