/**
 * \file httpplugin.cpp
 * \brief Plugin for parsing HTTP traffic
 * \author Jiri Havranek <havraji6@fit.cvut.cz>
 * \date 2015
 * \date 2016
 */
/*
 * Copyright (C) 2014-2016 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <unirec/unirec.h>

#include "packet.h"
#include "flowifc.h"
#include "httpplugin.h"

using namespace std;

//#define DEBUG_HTTP

// Print debug message if debugging is allowed.
#ifdef DEBUG_HTTP
#define DEBUG_MSG(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#else
#define DEBUG_MSG(format, ...)
#endif

// Process code if debugging is allowed.
#ifdef DEBUG_HTTP
#define DEBUG_CODE(code) code
#else
#define DEBUG_CODE(code)
#endif

#define HTTP_UNIREC_TEMPLATE  "HTTP_METHOD,HTTP_HOST,HTTP_URL,HTTP_USER_AGENT,HTTP_REFERER,HTTP_RESPONSE_CODE,HTTP_CONTENT_TYPE"
#define HTTP_LINE_DELIMITER   '\n'
#define HTTP_KEYVAL_DELIMITER ':'

UR_FIELDS (
   string HTTP_METHOD,
   string HTTP_HOST,
   string HTTP_URL,
   string HTTP_USER_AGENT,
   string HTTP_REFERER,

   uint16 HTTP_RESPONSE_CODE,
   string HTTP_CONTENT_TYPE
)

/**
 * \brief Constructor.
 * \param [in] options Module options.
 */
HTTPPlugin::HTTPPlugin(const options_t &module_options)
{
   print_stats = module_options.print_stats;
   requests = 0;
   responses = 0;
   total = 0;
   flush_flow = false;
}

HTTPPlugin::HTTPPlugin(const options_t &module_options, vector<plugin_opt> plugin_options) : FlowCachePlugin(plugin_options)
{
   print_stats = module_options.print_stats;
   requests = 0;
   responses = 0;
   total = 0;
   flush_flow = false;
}

int HTTPPlugin::post_create(FlowRecord &rec, const Packet &pkt)
{
   if (pkt.src_port == 80) {
      return add_ext_http_response(pkt.payload, pkt.payload_length, rec);
   } else if (pkt.dst_port == 80) {
      return add_ext_http_request(pkt.payload, pkt.payload_length, rec);
   }

   return 0;
}

int HTTPPlugin::pre_update(FlowRecord &rec, Packet &pkt)
{
   RecordExt *ext = NULL;
   if (pkt.src_port == 80) {
      ext = rec.getExtension(http_response);
      if (ext == NULL) { /* Check if header is present in flow. */
         return add_ext_http_response(pkt.payload, pkt.payload_length, rec);
      }

      parse_http_response(pkt.payload, pkt.payload_length, dynamic_cast<RecordExtHTTPResp *>(ext), false);
      if (flush_flow) {
         flush_flow = false;
         return FLOW_FLUSH;
      }
   } else if (pkt.dst_port == 80) {
      ext = rec.getExtension(http_request);
      if(ext == NULL) { /* Check if header is present in flow. */
         return add_ext_http_request(pkt.payload, pkt.payload_length, rec);
      }

      parse_http_request(pkt.payload, pkt.payload_length, dynamic_cast<RecordExtHTTPReq *>(ext), false);
      if (flush_flow) {
         flush_flow = false;
         return FLOW_FLUSH;
      }
   }

   return 0;
}

void HTTPPlugin::finish()
{
   if (print_stats) {
      cout << "HTTP plugin stats:" << endl;
      cout << "   Parsed http requests: " << requests << endl;
      cout << "   Parsed http responses: " << responses << endl;
      cout << "   Total http packets processed: " << total << endl;
   }
}

string HTTPPlugin::get_unirec_field_string()
{
   return HTTP_UNIREC_TEMPLATE;
}

/**
 * \brief Copy string and append \0 character.
 * NOTE: function removes any CR chars at the end of string.
 * \param [in] dst Destination buffer.
 * \param [in] size Size of destination buffer.
 * \param [in] begin Ptr to begin of source string.
 * \param [in] end Ptr to end of source string.
 */
void copy_str(char *dst, ssize_t size, const char *begin, const char *end)
{
   ssize_t len = end - begin;
   if (len >= size) {
      len = size - 1;
   }

   memcpy(dst, begin, len);

   if (len != 0 && dst[len - 1] == '\r') {
      len--;
   }
   dst[len] = 0;
}

#ifdef DEBUG_HTTP
static uint32_t s_requests = 0, s_responses = 0;
#endif /* DEBUG_HTTP */

/**
 * \brief Parse and store http request.
 * \param [in] data Packet payload data.
 * \param [in] payload_len Length of packet payload.
 * \param [out] rec Variable where http request will be stored.
 * \param [in] create Indicates if plugin is creating new http request or just updates old one.
 * \return True if request was parsed, false if error occured.
 */
bool HTTPPlugin::parse_http_request(const char *data, int payload_len, RecordExtHTTPReq *rec, bool create)
{
   char buffer[64];
   const char *begin, *end, *keyval_delimiter;

   total++;

   DEBUG_MSG("---------- http parser #%u ----------\n", total);
   DEBUG_MSG("Parsing request number: %u\n", ++s_requests);
   DEBUG_MSG("Payload length: %u\n\n",       payload_len);

   if (payload_len == 0) {
      DEBUG_MSG("Parser quits:\tpayload length = 0\n");
      return false;
   }

   /* Request line:
    *
    * METHOD URI VERSION
    * |     |   |
    * |     |   -------- end
    * |     ------------ begin
    * ----- ------------ data
    */

   /* Find begin of URI. */
   begin = strchr(data, ' ');
   if (begin == NULL) {
      DEBUG_MSG("Parser quits:\tnot a http request header\n");
      return false;
   }

   /* Find end of URI. */
   end = strchr(begin + 1, ' ');
   if (end == NULL) {
      DEBUG_MSG("Parser quits:\trequest is fragmented\n");
      return false;
   }

   /* Copy and check HTTP method */
   copy_str(rec->httpReqMethod, sizeof(rec->httpReqMethod), data, begin);
   if (!valid_http_method(rec->httpReqMethod)) {
      DEBUG_MSG("Parser quits:\tundefined http method: %s\n", rec->httpReqMethod);
      return false;
   }

   if (!create) {
      flush_flow = true;
      total--;
      DEBUG_MSG("Parser quits:\tflushing flow\n");
      return false;
   }

   copy_str(rec->httpReqUrl, sizeof(rec->httpReqUrl), begin + 1, end);
   DEBUG_MSG("\tMethod: %s\n",   rec->httpReqMethod);
   DEBUG_MSG("\tUrl: %s\n",      rec->httpReqUrl);

   /* Find begin of next line after request line. */
   begin = strchr(end, HTTP_LINE_DELIMITER);
   if (begin == NULL) {
      DEBUG_MSG("Parser quits:\tNo line delim after request line\n");
      return false;
   }
   begin++;

   /* Header:
    *
    * REQ-FIELD: VALUE
    * |        |      |
    * |        |      ----- end
    * |        ------------ keyval_delimiter
    * --------------------- begin
    */

   /* Process headers. */
   while (begin - data < payload_len) {
      end = strchr(begin, HTTP_LINE_DELIMITER);
      keyval_delimiter = strchr(begin, HTTP_KEYVAL_DELIMITER);

      int tmp = end - begin;
      if (tmp == 0 || tmp == 1) { /* Check for blank line with \r\n or \n ending. */
         break; /* Double LF found - end of header section. */
      } else if (end == NULL || keyval_delimiter == NULL) {
         DEBUG_MSG("Parser quits:\theader is fragmented\n");
         return  false;
      }

      /* Copy field name. */
      copy_str(buffer, sizeof(buffer), begin, keyval_delimiter);

      DEBUG_CODE(char debug_buffer[4096]);
      DEBUG_CODE(copy_str(debug_buffer, sizeof(debug_buffer), keyval_delimiter + 2, end));
      DEBUG_MSG("\t%s: %s\n", buffer, debug_buffer);

      /* Copy interesting field values. */
      if (!strcmp(buffer, "Host")) {
         copy_str(rec->httpReqHost, sizeof(rec->httpReqHost), keyval_delimiter + 2, end);
      } else if (!strcmp(buffer, "User-Agent")) {
         copy_str(rec->httpReqUserAgent, sizeof(rec->httpReqUserAgent), keyval_delimiter + 2, end);
      } else if (!strcmp(buffer, "Referer")) {
         copy_str(rec->httpReqReferer, sizeof(rec->httpReqReferer), keyval_delimiter + 2, end);
      }

      /* Go to next line. */
      begin = end + 1 ;
   }

   DEBUG_MSG("Parser quits:\tend of header section\n");
   requests++;
   return true;
}

/**
 * \brief Parse and store http response.
 * \param [in] data Packet payload data.
 * \param [in] payload_len Length of packet payload.
 * \param [out] rec Variable where http response will be stored.
 * \param [in] create Indicates if plugin is creating new http response or just updates old one.
 * \return True if request was parsed, false if error occured.
 */
bool HTTPPlugin::parse_http_response(const char *data, int payload_len, RecordExtHTTPResp *rec, bool create)
{
   char buffer[64];
   const char *begin, *end, *keyval_delimiter;

   total++;

   DEBUG_MSG("---------- http parser #%u ----------\n", total);
   DEBUG_MSG("Parsing response number: %u\n",   ++s_responses);
   DEBUG_MSG("Payload length: %u\n\n",          payload_len);

   if (payload_len == 0) {
      DEBUG_MSG("Parser quits:\tpayload length = 0\n");
      return false;
   }

   /* Check begin of response header. */
   if (memcmp(data, "HTTP", 4)) {
      DEBUG_MSG("Parser quits:\tpacket contains http response data\n");
      return false;
   }

   /* Response line:
    *
    * VERSION CODE REASON
    * |      |    |
    * |      |    --------- end
    * |      -------------- begin
    * --------------------- data
    */

   /* Find begin of status code. */
   begin = strchr(data, ' ');
   if (begin == NULL) {
      DEBUG_MSG("Parser quits:\tnot a http response header\n");
      return false;
   }

   /* Find end of status code. */
   end = strchr(begin + 1, ' ');
   if (end == NULL) {
      DEBUG_MSG("Parser quits:\tresponse is fragmented\n");
      return false;
   }

   /* Copy and check HTTP response code. */
   copy_str(buffer, sizeof(buffer), begin + 1, end);
   rec->httpRespCode = atoi(buffer);
   if (rec->httpRespCode <= 0) {
      DEBUG_MSG("Parser quits:\twrong response code: %d\n", rec->httpRespCode);
      return false;
   }

   DEBUG_MSG("\tCode: %d\n", rec->httpRespCode);

   if (!create) {
      flush_flow = true;
      total--;
      DEBUG_MSG("Parser quits:\tflushing flow\n");
      return false;
   }

   /* Find begin of next line after request line. */
   begin = strchr(end, HTTP_LINE_DELIMITER);
   if (begin == NULL) {
      DEBUG_MSG("Parser quits:\tNo line delim after request line\n");
      return false;
   }
   begin++;

   /* Header:
    *
    * REQ-FIELD: VALUE
    * |        |      |
    * |        |      ----- end
    * |        ------------ keyval_delimiter
    * --------------------- begin
    */

   /* Process headers. */
   while (begin - data < payload_len) {
      end = strchr(begin, HTTP_LINE_DELIMITER);
      keyval_delimiter = strchr(begin, HTTP_KEYVAL_DELIMITER);

      int tmp = end - begin;
      if (tmp == 0 || tmp == 1) { /* Check for blank line with \r\n or \n ending. */
         break; /* Double LF found - end of header section. */
      } else if (end == NULL || keyval_delimiter == NULL) {
         DEBUG_MSG("Parser quits:\theader is fragmented\n");
         return  false;
      }

      /* Copy field name. */
      copy_str(buffer, sizeof(buffer), begin, keyval_delimiter);

      DEBUG_CODE(char debug_buffer[4096]);
      DEBUG_CODE(copy_str(debug_buffer, sizeof(debug_buffer), keyval_delimiter + 2, end));
      DEBUG_MSG("\t%s: %s\n", buffer, debug_buffer);

      /* Copy interesting field values. */
      if (!strcmp(buffer, "Content-Type")) {
         copy_str(rec->httpRespContentType, sizeof(rec->httpRespContentType), keyval_delimiter + 2, end);
      }

      /* Go to next line. */
      begin = end + 1 ;
   }

   DEBUG_MSG("Parser quits:\tend of header section\n");
   responses++;
   return true;
}

/**
 * \brief Check http method.
 * \param [in] method C string with http method.
 * \return True if http method is valid.
 */
bool HTTPPlugin::valid_http_method(const char *method) const
{
   return (!strcmp(method, "GET")   || !strcmp(method, "POST")    ||
         !strcmp(method, "PUT")     || !strcmp(method, "HEAD")    ||
         !strcmp(method, "DELETE")  || !strcmp(method, "TRACE")   ||
         !strcmp(method, "OPTIONS") || !strcmp(method, "CONNECT") ||
         !strcmp(method, "PATCH"));
}

/**
 * \brief Add new extension http request header into flow record.
 * \param [in] data Packet payload data.
 * \param [in] payload_len Length of packet payload.
 * \param [out] rec Flow record where to store created extension header.
 * \return 0 on success.
 */
int HTTPPlugin::add_ext_http_request(const char *data, int payload_len, FlowRecord &rec)
{
   RecordExtHTTPReq *req = new RecordExtHTTPReq();
   if (!parse_http_request(data, payload_len, req, true)) {
      delete req;
   } else {
      rec.addExtension(req);
   }

   return 0;
}

/**
 * \brief Add new extension http response header into flow record.
 * \param [in] data Packet payload data.
 * \param [in] payload_len Length of packet payload.
 * \param [out] rec Flow record where to store created extension header.
 * \return 0 on success.
 */
int HTTPPlugin::add_ext_http_response(const char *data, int payload_len, FlowRecord &rec)
{
   RecordExtHTTPResp *resp = new RecordExtHTTPResp();
   if (!parse_http_response(data, payload_len, resp, true)) {
      delete resp;
   } else {
      rec.addExtension(resp);
   }

   return 0;
}

