#include "irods/plugins/auth/native.hpp"
#include "irods/authentication_plugin_framework.hpp"

#include "irods/authCheck.h"
#include "irods/authPluginRequest.h"
#include "irods/authRequest.h"
#include "irods/authResponse.h"
#include "irods/authenticate.h"
#include "irods/base64.hpp"
#include "irods/irods_auth_constants.hpp"
#include "irods/irods_auth_plugin.hpp"
#include "irods/irods_logger.hpp"
#include "irods/irods_stacktrace.hpp"
#include "irods/miscServerFunct.hpp"
#include "irods/msParam.h"
#include "irods/rcConnect.h"
#include "irods/rodsDef.h"

#ifdef RODS_SERVER
#include "irods/irods_rs_comm_query.hpp"
#include "irods/rsAuthCheck.hpp"
#include "irods/rsAuthRequest.hpp"
#endif // RODS_SERVER

#include <openssl/md5.h>

#include <termios.h>
#include <unistd.h>
#include <iostream>
#include <sstream>

int get64RandomBytes( char *buf );
void setSessionSignatureClientside( char* _sig );
void _rsSetAuthRequestGetChallenge( const char* _c );

using json = nlohmann::json;
using log_auth = irods::experimental::log::authentication;
namespace irods_auth = irods::experimental::auth;

namespace irods
{
    class native_authentication : public irods_auth::authentication_base {
    public:
        native_authentication()
        {
            add_operation(AUTH_ESTABLISH_CONTEXT,    OPERATION(rcComm_t, native_auth_establish_context));
            add_operation(AUTH_CLIENT_AUTH_REQUEST,  OPERATION(rcComm_t, native_auth_client_request));
            add_operation(AUTH_CLIENT_AUTH_RESPONSE, OPERATION(rcComm_t, native_auth_client_response));
#ifdef RODS_SERVER
            add_operation(AUTH_AGENT_START,          OPERATION(rsComm_t, native_auth_agent_start));
            add_operation(AUTH_AGENT_AUTH_REQUEST,   OPERATION(rsComm_t, native_auth_agent_request));
            add_operation(AUTH_AGENT_AUTH_RESPONSE,  OPERATION(rsComm_t, native_auth_agent_response));
            add_operation(AUTH_AGENT_AUTH_VERIFY,    OPERATION(rsComm_t, native_auth_agent_verify));
#endif
        } // ctor

    private:
        json auth_client_start(rcComm_t& comm, const json& req)
        {
            json resp{req};
            resp[irods_auth::next_operation] = AUTH_CLIENT_AUTH_REQUEST;
            resp["user_name"] = comm.proxyUser.userName;
            resp["zone_name"] = comm.proxyUser.rodsZone;

            return resp;
        } // auth_client_start

        json native_auth_establish_context(rcComm_t&, const json& req)
        {
            irods_auth::throw_if_request_message_is_missing_key(
                req, {"user_name", "zone_name", "request_result"}
            );

            json resp{req};

            auto request_result = req.at("request_result").get<std::string>();
            request_result.resize(CHALLENGE_LEN);

            // build a buffer for the challenge hash
            // Buffer structure:
            //  [64+1] challenge string (generated by server and sent back here)
            //  [50+1] obfuscated user password
            char md5_buf[CHALLENGE_LEN + MAX_PASSWORD_LEN + 2]{};
            strncpy(md5_buf, request_result.c_str(), CHALLENGE_LEN);

            // Save a representation of some of the challenge string for use as a session signature
            setSessionSignatureClientside(md5_buf);

            // determine if a password challenge is needed, are we anonymous or not?
            bool need_password = false;
            if (req.at("user_name").get_ref<const std::string&>() == ANONYMOUS_USER) {
                md5_buf[CHALLENGE_LEN + 1] = '\0';
            }
            else {
                need_password = obfGetPw(md5_buf + CHALLENGE_LEN);
            }

            // prompt for a password if necessary
            if (need_password) {
                struct termios tty;
                memset( &tty, 0, sizeof( tty ) );
                tcgetattr( STDIN_FILENO, &tty );
                tcflag_t oldflag = tty.c_lflag;
                tty.c_lflag &= ~ECHO;
                int error = tcsetattr( STDIN_FILENO, TCSANOW, &tty );
                int errsv = errno;

                if (error) {
                    fmt::print("WARNING: Error {} disabling echo mode. "
                               "Password will be displayed in plaintext.\n", errsv);
                }
                fmt::print("Enter your current iRODS password:");
                std::string password{};
                getline(std::cin, password);
                strncpy(md5_buf + CHALLENGE_LEN, password.c_str(), MAX_PASSWORD_LEN);
                fmt::print("\n");
                tty.c_lflag = oldflag;
                if (tcsetattr(STDIN_FILENO, TCSANOW, &tty)) {
                    fmt::print("Error reinstating echo mode.");
                }
            }

            // create a md5 hash of the challenge
            MD5_CTX context;
            MD5_Init( &context );
            MD5_Update(&context, reinterpret_cast<unsigned char*>(md5_buf), CHALLENGE_LEN + MAX_PASSWORD_LEN);

            char digest[RESPONSE_LEN + 2]{};
            MD5_Final(reinterpret_cast<unsigned char*>(digest), &context);

            // make sure 'string' doesn't end early - scrub out any errant terminating chars
            // by incrementing their value by one
            for (int i = 0; i < RESPONSE_LEN; ++i) {
                if (digest[i] == '\0') {
                    digest[i]++;
                }
            }

            unsigned char out[RESPONSE_LEN*2];
            unsigned long out_len{RESPONSE_LEN*2};
            auto err = base64_encode(reinterpret_cast<unsigned char*>(digest), RESPONSE_LEN, out, &out_len);
            if(err < 0) {
                THROW(err, "base64 encoding of digest failed.");
            }

            resp["digest"] = std::string{reinterpret_cast<char*>(out), out_len};
            resp[irods_auth::next_operation] = AUTH_CLIENT_AUTH_RESPONSE;

            return resp;
        } // native_auth_establish_context

        json native_auth_client_request(rcComm_t& comm, const json& req)
        {
            json svr_req{req};
            svr_req[irods_auth::next_operation] = AUTH_AGENT_AUTH_REQUEST;
            auto resp = irods_auth::request(comm, svr_req);

            resp[irods_auth::next_operation] = AUTH_ESTABLISH_CONTEXT;

            return resp;
        } // native_auth_client_request

        json native_auth_client_response(rcComm_t& comm, const json& req)
        {
            irods_auth::throw_if_request_message_is_missing_key(
                req, {"digest", "user_name", "zone_name"}
            );

            json svr_req{req};
            svr_req[irods_auth::next_operation] = AUTH_AGENT_AUTH_RESPONSE;
            auto resp = irods_auth::request(comm, svr_req);

            comm.loggedIn = 1;

            resp[irods_auth::next_operation] = irods_auth::flow_complete;

            return resp;
        } // native_auth_client_response

#ifdef RODS_SERVER
        json native_auth_agent_request(rsComm_t& comm, const json& req)
        {
            json resp{req};

            char buf[CHALLENGE_LEN + 2]{};
            get64RandomBytes( buf );

            resp["request_result"] = buf;

            _rsSetAuthRequestGetChallenge(buf);

            if (comm.auth_scheme) {
                free(comm.auth_scheme);
            }

            comm.auth_scheme = strdup(irods_auth::scheme::native);

            return resp;
        } // native_auth_agent_request

        json native_auth_agent_response(rsComm_t& comm, const json& req)
        {
            irods_auth::throw_if_request_message_is_missing_key(
                req, {"digest", "zone_name", "user_name"}
            );

            // need to do NoLogin because it could get into inf loop for cross zone auth
            rodsServerHost_t *rodsServerHost;
            auto zone_name = req.at("zone_name").get<std::string>();
            int status =
                getAndConnRcatHostNoLogin(&comm, PRIMARY_RCAT, const_cast<char*>(zone_name.c_str()), &rodsServerHost);
            if ( status < 0 ) {
                THROW(status, "Connecting to rcat host failed.");
            }

            char* response = static_cast<char*>(malloc(RESPONSE_LEN + 1));
            std::memset(response, 0, RESPONSE_LEN + 1);
            const auto free_response = irods::at_scope_exit{[response] { free(response); }};

            response[RESPONSE_LEN] = 0;

            unsigned long out_len = RESPONSE_LEN;
            auto to_decode = req.at("digest").get<std::string>();
            auto err = base64_decode(reinterpret_cast<unsigned char*>(const_cast<char*>(to_decode.c_str())),
                                     to_decode.size(),
                                     reinterpret_cast<unsigned char*>(response),
                                     &out_len);
            if (err < 0) {
                THROW(err, "base64 decoding of digest failed.");
            }

            authCheckInp_t authCheckInp{};
            authCheckInp.challenge = _rsAuthRequestGetChallenge();
            authCheckInp.response = response;

            const std::string username = fmt::format(
                    "{}#{}", req.at("user_name").get_ref<const std::string&>(), zone_name);
            authCheckInp.username = const_cast<char*>(username.data());

            authCheckOut_t* authCheckOut = nullptr;
            if (LOCAL_HOST == rodsServerHost->localFlag) {
                status = rsAuthCheck(&comm, &authCheckInp, &authCheckOut);
            }
            else {
                status = rcAuthCheck(rodsServerHost->conn, &authCheckInp, &authCheckOut);
                /* not likely we need this connection again */
                rcDisconnect(rodsServerHost->conn);
                rodsServerHost->conn = nullptr;
            }

            if (status < 0 || !authCheckOut) {
                THROW(status, "rcAuthCheck failed.");
            }

            json resp{req};

            // Do we need to consider remote zones here?
            if (LOCAL_HOST != rodsServerHost->localFlag) {
                if (!authCheckOut->serverResponse) {
                    log_auth::info("Warning, cannot authenticate remote server, no serverResponse field");
                    THROW(REMOTE_SERVER_AUTH_NOT_PROVIDED, "Authentication disallowed. no serverResponse field.");
                }

                if (*authCheckOut->serverResponse == '\0') {
                    log_auth::info("Warning, cannot authenticate remote server, serverResponse field is empty");
                    THROW(REMOTE_SERVER_AUTH_EMPTY, "Authentication disallowed, empty serverResponse.");
                }

                char md5Buf[CHALLENGE_LEN + MAX_PASSWORD_LEN + 2]{};
                strncpy(md5Buf, authCheckInp.challenge, CHALLENGE_LEN);

                char userZone[NAME_LEN + 2]{};
                strncpy(userZone, zone_name.data(), NAME_LEN + 1);

                char serverId[MAX_PASSWORD_LEN + 2]{};
                getZoneServerId(userZone, serverId);

                if ('\0' == serverId[0]) {
                    log_auth::info("rsAuthResponse: Warning, cannot authenticate the remote server, no RemoteZoneSID defined in server_config.json");
                    THROW(REMOTE_SERVER_SID_NOT_DEFINED, "Authentication disallowed, no RemoteZoneSID defined");
                }

                strncpy(md5Buf + CHALLENGE_LEN, serverId, strlen(serverId));

                char digest[RESPONSE_LEN + 2]{};
                obfMakeOneWayHash(
                    HASH_TYPE_DEFAULT,
                    ( unsigned char* )md5Buf,
                    CHALLENGE_LEN + MAX_PASSWORD_LEN,
                    ( unsigned char* )digest );

                for (int i = 0; i < RESPONSE_LEN; i++) {
                    if (digest[i] == '\0') {
                        digest[i]++;
                    }  /* make sure 'string' doesn't end early*/
                }

                char* cp = authCheckOut->serverResponse;

                for (int i = 0; i < RESPONSE_LEN; i++) {
                    if ( *cp++ != digest[i] ) {
                        THROW(REMOTE_SERVER_AUTHENTICATION_FAILURE, "Authentication disallowed, server response incorrect.");
                    }
                }
            }

            /* Set the clientUser zone if it is null. */
            if ('\0' == comm.clientUser.rodsZone[0]) {
                zoneInfo_t* tmpZoneInfo{};
                status = getLocalZoneInfo( &tmpZoneInfo );
                if ( status < 0 ) {
                    THROW(status, "getLocalZoneInfo failed.");
                }
                else {
                    strncpy(comm.clientUser.rodsZone, tmpZoneInfo->zoneName, NAME_LEN);
                }
            }

            /* have to modify privLevel if the icat is a foreign icat because
             * a local user in a foreign zone is not a local user in this zone
             * and vice versa for a remote user
             */
            if (rodsServerHost->rcatEnabled == REMOTE_ICAT ) {
                /* proxy is easy because rodsServerHost is based on proxy user */
                if ( authCheckOut->privLevel == LOCAL_PRIV_USER_AUTH ) {
                    authCheckOut->privLevel = REMOTE_PRIV_USER_AUTH;
                }
                else if ( authCheckOut->privLevel == LOCAL_USER_AUTH ) {
                    authCheckOut->privLevel = REMOTE_USER_AUTH;
                }

                /* adjust client user */
                if ( 0 == strcmp(comm.proxyUser.userName, comm.clientUser.userName ) ) {
                    authCheckOut->clientPrivLevel = authCheckOut->privLevel;
                }
                else {
                    zoneInfo_t *tmpZoneInfo;
                    status = getLocalZoneInfo( &tmpZoneInfo );
                    if ( status < 0 ) {
                        THROW(status, "getLocalZoneInfo failed.");
                    }
                    else {
                        if ( 0 == strcmp( tmpZoneInfo->zoneName, comm.clientUser.rodsZone ) ) {
                            /* client is from local zone */
                            if ( REMOTE_PRIV_USER_AUTH == authCheckOut->clientPrivLevel ) {
                                authCheckOut->clientPrivLevel = LOCAL_PRIV_USER_AUTH;
                            }
                            else if ( REMOTE_USER_AUTH == authCheckOut->clientPrivLevel ) {
                                authCheckOut->clientPrivLevel = LOCAL_USER_AUTH;
                            }
                        }
                        else {
                            /* client is from remote zone */
                            if ( LOCAL_PRIV_USER_AUTH == authCheckOut->clientPrivLevel ) {
                                authCheckOut->clientPrivLevel = REMOTE_USER_AUTH;
                            }
                            else if ( LOCAL_USER_AUTH == authCheckOut->clientPrivLevel ) {
                                authCheckOut->clientPrivLevel = REMOTE_USER_AUTH;
                            }
                        }
                    }
                }
            }
            else if ( 0 == strcmp(comm.proxyUser.userName,  comm.clientUser.userName ) ) {
                authCheckOut->clientPrivLevel = authCheckOut->privLevel;
            }

            irods::throw_on_insufficient_privilege_for_proxy_user(comm, authCheckOut->privLevel);

            log_auth::debug(
                    "rsAuthResponse set proxy authFlag to {}, client authFlag to {}, user:{} proxy:{} client:{}",
                    authCheckOut->privLevel,
                    authCheckOut->clientPrivLevel,
                    authCheckInp.username,
                    comm.proxyUser.userName,
                    comm.clientUser.userName);

            if ( strcmp(comm.proxyUser.userName, comm.clientUser.userName ) != 0 ) {
                comm.proxyUser.authInfo.authFlag = authCheckOut->privLevel;
                comm.clientUser.authInfo.authFlag = authCheckOut->clientPrivLevel;
            }
            else {          /* proxyUser and clientUser are the same */
                comm.proxyUser.authInfo.authFlag =
                    comm.clientUser.authInfo.authFlag = authCheckOut->privLevel;
            }

            if ( authCheckOut != NULL ) {
                if ( authCheckOut->serverResponse != NULL ) {
                    free( authCheckOut->serverResponse );
                }
                free( authCheckOut );
            }

            return resp;
        } // native_auth_agent_response

        // =-=-=-=-=-=-=-
        // stub for ops that the native plug does
        // not need to support
        json native_auth_agent_verify(rsComm_t&, const json&)
        {
            return {};
        } // native_auth_agent_verify

        // =-=-=-=-=-=-=-
        // stub for ops that the native plug does
        // not need to support
        json native_auth_agent_start(rsComm_t&, const json&)
        {
            return {};
        } // native_auth_agent_start
#endif
    }; // class native_authentication
} // namespace irods

extern "C"
irods::native_authentication* plugin_factory(const std::string&, const std::string&)
{
    return new irods::native_authentication{};
}

