/*******************************************************************************
* This program and the accompanying materials
* are made available under the terms of the Common Public License v1.0
* which accompanies this distribution, and is available at 
* http://www.eclipse.org/legal/cpl-v10.html
* 
* Contributors:
*     Peter Smith
*******************************************************************************/

#include "HttpProtocol.h"
#include "JSONCodec.h"
#include "../common/Log.h"
#include "XLUtil.h"

#define FS_URL          ":url"
#define FS_PROXY        ":proxy"
#define USER_AGENT      L"XLLoop/Http v0.1.0"
#define BUFFER_SIZE     8192

#define REQ_TYPE_NAME   "request"
#define REQ_TYPE_VAL    "XLLoop"
#define REQ_VER_NAME    "version"
#define REQ_VER_VAL     "0.1.0"
#define REQ_CALLER_NAME "caller"
#define REQ_SHEET_NAME  "sheet"
#define REQ_NAME_NAME   "name"
#define REQ_ARGS_NAME   "args"

//Steven's added proxy code
bool proxy_exists = false;
bool proxy_failed = false;
bool nonproxy_attempted = false;
char* username = "";
char* password = "";

//LPCWSTR username;
//LPCWSTR password;

typedef struct _ctx {
	HINTERNET hConnect;
	HINTERNET hRequest;
	HANDLE hEvent;
	yajl_gen_config conf;
	yajl_gen g;
	LPXLOPER px;
} REQUEST_CONTEXT;


DWORD ChooseAuthScheme(DWORD dwSupportedSchemes)
{
	//  It is the server's responsibility only to accept 
	//  authentication schemes that provide a sufficient
	//  level of security to protect the servers resources.
	//
	//  The client is also obligated only to use an authentication
	//  scheme that adequately protects its username and password.
	//
	//  Thus, this sample code does not use Basic authentication  
	//  becaus Basic authentication exposes the client's username
	//  and password to anyone monitoring the connection.

	if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_NEGOTIATE)
		return WINHTTP_AUTH_SCHEME_NEGOTIATE;
	else if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_NTLM)
		return WINHTTP_AUTH_SCHEME_NTLM;
	else if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_PASSPORT)
		return WINHTTP_AUTH_SCHEME_PASSPORT;
	else if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_DIGEST)
		return WINHTTP_AUTH_SCHEME_DIGEST;

	//Added basic auth -SP

	else if (dwSupportedSchemes & WINHTTP_AUTH_SCHEME_BASIC)
		return WINHTTP_AUTH_SCHEME_BASIC;

	else
		return 0;
}


VOID CALLBACK CallBack(HINTERNET session, DWORD_PTR context, DWORD status, LPVOID statusInfo, DWORD statusInfoLen);

void GenerateRequest(yajl_gen g, const char* fn, bool sendCaller, LPXLOPER* argv, int argc)
{
	yajl_gen_map_open(g);
	yajl_gen_string(g, (const unsigned char*) REQ_TYPE_NAME, strlen(REQ_TYPE_NAME));
	yajl_gen_string(g, (const unsigned char*) REQ_TYPE_VAL, strlen(REQ_TYPE_VAL));
	yajl_gen_string(g, (const unsigned char*) REQ_VER_NAME, strlen(REQ_VER_NAME));
	yajl_gen_string(g, (const unsigned char*) REQ_VER_VAL, strlen(REQ_VER_VAL));
	if(sendCaller) {
		XLOPER xlRef, xlSheetName;
		Excel4(xlfCaller, &xlRef, 0);	
		Excel4(xlSheetNm, &xlSheetName, 1, &xlRef);
		if((xlRef.xltype & ~(xlbitXLFree | xlbitDLLFree)) == xltypeSRef) {
			yajl_gen_string(g, (const unsigned char*) REQ_CALLER_NAME, strlen(REQ_CALLER_NAME));
			JSONCodec::Encode(g, &xlRef);
		}
		if((xlSheetName.xltype & ~(xlbitXLFree | xlbitDLLFree)) == xltypeStr && xlSheetName.val.str) {
			yajl_gen_string(g, (const unsigned char*) REQ_SHEET_NAME, strlen(REQ_SHEET_NAME));
			yajl_gen_string(g, (const unsigned char*) &xlSheetName.val.str[1], xlSheetName.val.str[0]);
			Excel4(xlFree, 0, 1, &xlSheetName);
		}
	}
	yajl_gen_string(g, (const unsigned char*) REQ_NAME_NAME, strlen(REQ_NAME_NAME));
	yajl_gen_string(g, (const unsigned char*) fn, strlen(fn));
	yajl_gen_string(g, (const unsigned char*) REQ_ARGS_NAME, strlen(REQ_ARGS_NAME));
	yajl_gen_array_open(g);

	// Find last non-missing value
	while(argc>0) {
		if((argv[argc-1]->xltype & ~(xlbitXLFree | xlbitDLLFree)) == xltypeMissing)
			argc--;
		else
			break;
	}

	for(int i = 0; i < argc; i++) {
		JSONCodec::Encode(g, argv[i]);
	}
	yajl_gen_array_close(g);
	yajl_gen_map_close(g);
}

HttpProtocol::~HttpProtocol()
{
	WinHttpCloseHandle(hSession);
	if(url) free(url);
	if(host) free(host);
	if(path) free(path);
	//if (proxy_user) free(proxy_user);
	//if (proxy_pw) free(proxy_pw);
	//if (server_user) free(server_user);
	//if (server_pw) free(server_pw);
}



void HttpProtocol::initialize(dictionary* ini, const char* section)
{
	char* url = INI::GetString(ini, section, FS_URL, NULL);
	if(!url) {
		Log::Error("Missing URL for HTTP protocol");
		return;
	}
	Log::Info("Setup session for: %s", url);
	int usz = MultiByteToWideChar(CP_ACP, 0, url, strlen(url), 0, 0);
	this->url = (wchar_t*) malloc((usz + 1) * sizeof(wchar_t));
	MultiByteToWideChar(CP_ACP, 0, url, strlen(url), this->url, usz);
	this->url[usz] = 0;
	memset(&urlc, 0, sizeof(URL_COMPONENTS));
	urlc.dwStructSize = sizeof(URL_COMPONENTS);
	urlc.dwSchemeLength = 1;
	urlc.dwHostNameLength = 1;
	urlc.dwUserNameLength = 1;
	urlc.dwPasswordLength = 1;
	urlc.dwUrlPathLength = 1;
	urlc.dwExtraInfoLength = 1;
	WinHttpCrackUrl(this->url, usz, 0, &urlc);
	this->host = (wchar_t*) malloc((urlc.dwHostNameLength + 1) * sizeof(wchar_t));
	memcpy(this->host, urlc.lpszHostName, urlc.dwHostNameLength*sizeof(wchar_t));
	this->host[urlc.dwHostNameLength] = 0;
	this->path = (wchar_t*) malloc((urlc.dwUrlPathLength + 1) * sizeof(wchar_t));
	memcpy(this->path, urlc.lpszUrlPath, urlc.dwUrlPathLength*sizeof(wchar_t));
	this->path[urlc.dwUrlPathLength] = 0;

	// Determine proxy
	char* proxy = INI::GetString(ini, section, FS_PROXY, NULL);

	char* puser = INI::GetString(ini, section, ":proxy_username", NULL);
	char* ppw = INI::GetString(ini, section, ":proxy_password", NULL);

	char* suser = INI::GetString(ini, section, ":server_username", NULL);
	char* spw = INI::GetString(ini, section, ":server_password", NULL);
	
	if (puser) {
		
		int psz = MultiByteToWideChar(CP_ACP, 0, puser, strlen(puser), 0, 0);
		wchar_t* wpuser = (wchar_t*)malloc((psz + 1) * sizeof(wchar_t));
		MultiByteToWideChar(CP_ACP, 0, puser, strlen(puser), wpuser, psz);
		wpuser[psz] = 0;

		this->proxy_user = wpuser;


	}
	if (ppw) {

		int psz = MultiByteToWideChar(CP_ACP, 0, ppw, strlen(ppw), 0, 0);
		wchar_t* wppw = (wchar_t*)malloc((psz + 1) * sizeof(wchar_t));
		MultiByteToWideChar(CP_ACP, 0, ppw, strlen(ppw), wppw, psz);
		wppw[psz] = 0;

		this->proxy_pw = wppw;


	}

	if (suser) {

		int psz = MultiByteToWideChar(CP_ACP, 0, suser, strlen(suser), 0, 0);
		wchar_t* wsuser = (wchar_t*)malloc((psz + 1) * sizeof(wchar_t));
		MultiByteToWideChar(CP_ACP, 0, suser, strlen(suser), wsuser, psz);
		wsuser[psz] = 0;

		this->server_user = wsuser;


	}
	if (spw) {

		int psz = MultiByteToWideChar(CP_ACP, 0, spw, strlen(spw), 0, 0);
		wchar_t* wspw = (wchar_t*)malloc((psz + 1) * sizeof(wchar_t));
		MultiByteToWideChar(CP_ACP, 0, spw, strlen(spw), wspw, psz);
		wspw[psz] = 0;

		this->server_pw = wspw;


	}
	
	
	if(proxy) {
		Log::Info("Using proxy: %s", proxy);
		int psz = MultiByteToWideChar(CP_ACP, 0, url, strlen(url), 0, 0);
		wchar_t* wproxy = (wchar_t*) malloc((psz + 1) * sizeof(wchar_t));
		MultiByteToWideChar(CP_ACP, 0, proxy, strlen(proxy), wproxy, psz);
		wproxy[psz] = 0;
		hSession = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_NAMED_PROXY,
			wproxy, WINHTTP_NO_PROXY_BYPASS, 0);
		
		proxy_exists = true; //added
		free(wproxy);
	} else {

		nonproxy_attempted = true;

		WINHTTP_CURRENT_USER_IE_PROXY_CONFIG proxy;
		WinHttpGetIEProxyConfigForCurrentUser(&proxy);
		int proxyType = WINHTTP_ACCESS_TYPE_NO_PROXY;
		if(proxy.lpszProxy) {
			proxyType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
			Log::Info("Using proxy: %s", proxy.lpszProxy);
			proxy_exists = true; //added
		}
		hSession = WinHttpOpen(USER_AGENT, proxyType,
			proxy.lpszProxy, proxy.lpszProxyBypass, 0);
	}

	WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);
}

LPXLOPER HttpProtocol::execute(const char* name, bool sendCaller, int count, ...)
{
	va_list args;
	va_start(args, count);
	LPXLOPER res = Execute(name, sendCaller, (LPXLOPER*) args, count);
	va_end(args);
	return res;
}

LPXLOPER HttpProtocol::execute(const char* name, bool sendCaller, int count, LPXLOPER far opers[])
{
	return Execute(name, sendCaller, opers, count);
}

VOID ReadData(REQUEST_CONTEXT* context)
{
	yajl_handle hand;
	yajl_parser_config cfg = { 1, 1 };
	json_ctx ctx = { 0, 0 };
	hand = JSONCodec::AllocateHandle(&cfg, &ctx);

	unsigned char temp[BUFFER_SIZE];
	DWORD read = 0;
	BOOL res = FALSE;
	while(true) {
		res = WinHttpReadData(context->hRequest, temp, BUFFER_SIZE, &read);
		if(!res) {
			Log::Error("Error reading response data");
			break;
		}
		if(read == 0)
			break;
		temp[read] = 0;
		yajl_status status = yajl_parse(hand, temp, read);
		if(status == yajl_status_error) {
			Log::Error("Error parsing response data");
			res = FALSE;
			break;
		}
	}

	yajl_parse_complete(hand);
    yajl_free(hand);
	JSONCodec::Decode(ctx.current, context->px);
	JSONCodec::FreeJsonValue(ctx.current);
}

LPXLOPER HttpProtocol::Execute(const char* name, bool sendCaller, LPXLOPER* args, int argc)
{
	REQUEST_CONTEXT context;
	context.hEvent = CreateEvent(0, 1, 0, 0);
	context.hConnect = WinHttpConnect(hSession, host, urlc.nPort, 0);
	int flags = WINHTTP_FLAG_BYPASS_PROXY_CACHE;
	if(urlc.nScheme == INTERNET_SCHEME_HTTPS)
		flags |= WINHTTP_FLAG_SECURE;
	context.hRequest = WinHttpOpenRequest(context.hConnect, L"POST", path, 0, 0, 0, 
		flags);

	

	context.conf.beautify = 0;
	context.conf.indentString = "";
	context.g = yajl_gen_alloc(&context.conf, 0);
	context.px = (LPXLOPER) malloc(sizeof(XLOPER));
	context.px->xltype = xltypeNil | xlbitDLLFree;
	GenerateRequest(context.g, name, sendCaller, args, argc);
	const unsigned char * buf;
    unsigned int len = 0;
    yajl_gen_get_buf(context.g, &buf, &len);
	BOOL res = FALSE;
	
	if (proxy_exists) {
		//Added msdn check for proxy auth
		//--------------------------------------------

		DWORD dwStatusCode = 0;
		DWORD dwSupportedSchemes;
		DWORD dwFirstScheme;
		DWORD dwSelectedScheme;
		DWORD dwTarget;
		DWORD dwLastStatus = 0;
		DWORD dwSize = sizeof(DWORD);
		//BOOL  bResults = FALSE;
		BOOL  bDone = FALSE;
		DWORD dwProxyAuthScheme = 0;

		BOOL responseError = FALSE;

		if (context.hRequest == NULL)
			bDone = true;

		while (!bDone)
		{
			if (dwProxyAuthScheme != 0) {


				res = WinHttpSetCredentials(context.hRequest,
					WINHTTP_AUTH_TARGET_PROXY,
					dwProxyAuthScheme,
					this->proxy_user,
					this->proxy_pw,
					NULL);


			}

			res = WinHttpSendRequest(context.hRequest, 0, 0, (LPVOID)buf, len, len, (DWORD_PTR)&context);

			

			if (res)
				res = WinHttpReceiveResponse(context.hRequest, NULL);
			else
				responseError = TRUE;

			if (!res && GetLastError() == ERROR_WINHTTP_RESEND_REQUEST)
				continue;

			if (res)
				res = WinHttpQueryHeaders(context.hRequest,
					WINHTTP_QUERY_STATUS_CODE |
					WINHTTP_QUERY_FLAG_NUMBER,
					NULL,
					&dwStatusCode,
					&dwSize,
					NULL);


			if (res)
			{

				switch (dwStatusCode)

				{
				case 200:
					printf("resource received.\n");
					bDone = TRUE;
					break;
				case 401:
					// The server requires authentication.
					printf(" The server requires authentication. Sending credentials...\n");

					// Obtain the supported and preferred schemes.
					res = WinHttpQueryAuthSchemes(context.hRequest,
						&dwSupportedSchemes,
						&dwFirstScheme,
						&dwTarget);

					// Set the credentials before resending the request.
					if (res)
					{
						dwSelectedScheme = ChooseAuthScheme(dwSupportedSchemes);

						if (dwSelectedScheme == 0)
							bDone = TRUE;
						else
							res = WinHttpSetCredentials(context.hRequest,
								dwTarget,
								dwSelectedScheme,
								this->server_user,
								this->server_pw,
								NULL);
					}

					// If the same credentials are requested twice, abort the
					// request.  For simplicity, this sample does not check
					// for a repeated sequence of status codes.
					if (dwLastStatus == 401)
						bDone = TRUE;

					break;

				case 407:
					// The proxy requires authentication.
					printf("The proxy requires authentication.  Sending credentials...\n");

					// Obtain the supported and preferred schemes.
					res = WinHttpQueryAuthSchemes(context.hRequest,
						&dwSupportedSchemes,
						&dwFirstScheme,
						&dwTarget);

					// Set the credentials before resending the request.
					if (res)
						dwProxyAuthScheme = ChooseAuthScheme(dwSupportedSchemes);

					// If the same credentials are requested twice, abort the
					// request.  For simplicity, this sample does not check 
					// for a repeated sequence of status codes.
					if (dwLastStatus == 407)
						bDone = TRUE;
					break;

				default:
					// The status code does not indicate success.
					printf("Error. Status code %d returned.\n", dwStatusCode);
					bDone = TRUE;



				}
			}

			// Keep track of the last status code.
			dwLastStatus = dwStatusCode;

			// If there are any errors, break out of the loop.
			if (!res)
				bDone = TRUE;

		}

		//Added msdn check for proxy auth
		//--------------------------------------------


		//If proxy fails don't return, execute next block
		if (dwStatusCode != 200) {
			proxy_failed = true;
			proxy_exists = false; //Don't allow more attempted proxy connections if it fails.
			Log::Error("Status code: %d", dwStatusCode);
			const char* err = "#Server returned an error";
			context.px->val.str = XLUtil::MakeExcelString(err);
		}

		//If proxy and request both work just go ahead with request
		else {
			ReadData(&context);
			WinHttpCloseHandle(context.hRequest);
			WinHttpCloseHandle(context.hConnect);
			yajl_gen_clear(context.g);
			yajl_gen_free(context.g);
			context.px->xltype |= xlbitDLLFree;
			return context.px;
		}
	}
	
	if(!proxy_exists||proxy_failed) {


		//If the else block of the session with proxy was not executed before, try again now since
		//the proxy has failed. This requires that the session, connection, and contexst values be
		//redone.
		if (!nonproxy_attempted) {
			
			//Only allow this attempt once, otherwise sessions will repeat every time.
			nonproxy_attempted = true;

			WINHTTP_CURRENT_USER_IE_PROXY_CONFIG proxy;
			WinHttpGetIEProxyConfigForCurrentUser(&proxy);
			int proxyType = WINHTTP_ACCESS_TYPE_NO_PROXY;
			if (proxy.lpszProxy) {
				proxyType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
				Log::Info("Using proxy: %s", proxy.lpszProxy);
				proxy_exists = true; //added
			}
			this->hSession = WinHttpOpen(USER_AGENT, proxyType,
				proxy.lpszProxy, proxy.lpszProxyBypass, 0);


			WinHttpSetTimeouts(this->hSession, 10000, 10000, 10000, 10000);

			//REQUEST_CONTEXT context;
			context.hEvent = CreateEvent(0, 1, 0, 0);
			context.hConnect = WinHttpConnect(hSession, host, urlc.nPort, 0);
			int flags = WINHTTP_FLAG_BYPASS_PROXY_CACHE;
			if (urlc.nScheme == INTERNET_SCHEME_HTTPS)
				flags |= WINHTTP_FLAG_SECURE;
			context.hRequest = WinHttpOpenRequest(context.hConnect, L"POST", path, 0, 0, 0,
				flags);



			context.conf.beautify = 0;
			context.conf.indentString = "";
			context.g = yajl_gen_alloc(&context.conf, 0);
			context.px = (LPXLOPER)malloc(sizeof(XLOPER));
			context.px->xltype = xltypeNil | xlbitDLLFree;
			GenerateRequest(context.g, name, sendCaller, args, argc);
			const unsigned char * buf;
			unsigned int len = 0;
			yajl_gen_get_buf(context.g, &buf, &len);
			BOOL res = FALSE;
		}


		
		res = WinHttpSendRequest(context.hRequest, 0, 0, (LPVOID) buf, len, len, (DWORD_PTR) &context);




		if(!res) {
		
			const char* err = "#Could not connect to server";
			Log::Error(err);
			WinHttpCloseHandle(context.hRequest);
			WinHttpCloseHandle(context.hConnect);
			context.px->xltype = xltypeStr | xlbitDLLFree;
			context.px->val.str = XLUtil::MakeExcelString(err);
			return context.px;
		}
		// TODO timeout/background
		res = WinHttpReceiveResponse(context.hRequest, 0);
		if(!res) {
		
			const char* err = "#Error retrieving server response";
			Log::Error(err);
			WinHttpCloseHandle(context.hRequest);
			WinHttpCloseHandle(context.hConnect);
			context.px->xltype = xltypeStr | xlbitDLLFree;
			context.px->val.str = XLUtil::MakeExcelString(err);
			return context.px;
		}

		// Check http response code
		DWORD status;
		DWORD statusLength = 4;


		res = WinHttpQueryHeaders(context.hRequest, WINHTTP_QUERY_STATUS_CODE| WINHTTP_QUERY_FLAG_NUMBER,
			NULL, &status, &statusLength, 0);

		if(!res || status != 200) {
		
			Log::Error("Status code: %d", status);
			
			const char* err = "#Server returned an error";
			WinHttpCloseHandle(context.hRequest);
			WinHttpCloseHandle(context.hConnect);
			context.px->xltype = xltypeStr | xlbitDLLFree;
			context.px->val.str = XLUtil::MakeExcelString(err);
			return context.px;
		}
		ReadData(&context);
		WinHttpCloseHandle(context.hRequest);
		WinHttpCloseHandle(context.hConnect);
		yajl_gen_clear(context.g);
		yajl_gen_free(context.g);
		context.px->xltype |= xlbitDLLFree;
		return context.px;


	}

}


