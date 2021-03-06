/**
 * \file unirecexporter.cpp
 * \brief Flow exporter converting flows to UniRec and sending them to TRAP ifc
 * \author Vaclav Bartos <bartos@cesnet.cz>
 * \author Jiri Havranek <havraji6@fit.cvut.cz>
 * \date 2014
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

#include <string>
#include <vector>
#include <algorithm>
#include <libtrap/trap.h>
#include <unirec/unirec.h>

#include "unirecexporter.h"
#include "fields.h"
#include "flowexporter.h"
#include "flowifc.h"
#include "flow_meter.h"

using namespace std;

#define BASIC_FLOW_TEMPLATE "SRC_IP,DST_IP,SRC_PORT,DST_PORT,PROTOCOL,PACKETS,BYTES,TIME_FIRST,TIME_LAST,TCP_FLAGS,LINK_BIT_FIELD,DIR_BIT_FIELD,TOS,TTL"

#define PACKET_TEMPLATE "SRC_MAC,DST_MAC,ETHERTYPE,TIME"

UR_FIELDS (
   ipaddr DST_IP,
   ipaddr SRC_IP,
   uint64 BYTES,
   uint64 LINK_BIT_FIELD,
   time TIME_FIRST,
   time TIME_LAST,
   uint32 PACKETS,
   uint16 DST_PORT,
   uint16 SRC_PORT,
   uint8 DIR_BIT_FIELD,
   uint8 PROTOCOL,
   uint8 TCP_FLAGS,
   uint8 TOS,
   uint8 TTL,

   bytes SRC_MAC,
   bytes DST_MAC,
   uint16 ETHERTYPE
   time TIME,
)

/**
 * \brief Constructor.
 */
UnirecExporter::UnirecExporter() : out_ifc_cnt(0), tmplt(NULL), record(NULL)
{
}

/**
 * \brief Initialize exporter.
 * \param [in] plugins Active plugins.
 * \param [in] ifc_cnt Output interface count.
 * \param [in] basic_ifc_num Basic output interface number.
 * \return 0 on success or negative value when error occur.
 */
int UnirecExporter::init(const vector<FlowCachePlugin *> &plugins, int ifc_cnt, int basic_ifc_number)
{
   out_ifc_cnt = ifc_cnt;
   basic_ifc_num = basic_ifc_number;
   ifc_mapping.clear();

   tmplt = new ur_template_t*[out_ifc_cnt];
   record = new void*[out_ifc_cnt];

   for (int i = 0; i < out_ifc_cnt; i++) {
      tmplt[i] = NULL;
      record[i] = NULL;
   }

   char *error = NULL;
   if (basic_ifc_num >= 0) {
      tmplt[basic_ifc_num] = ur_create_output_template(basic_ifc_num, BASIC_FLOW_TEMPLATE, &error);
      if (tmplt[basic_ifc_num] == NULL) {
         fprintf(stderr, "UnirecExporter: %s\n", error);
         free(error);
         free_unirec_resources();
         return -2;
      }
   }

   string template_str;
   for (unsigned int i = 0; i < plugins.size(); i++) {
      FlowCachePlugin * const tmp = plugins[i];
      vector<plugin_opt> &opts = tmp->get_options();
      int ifc = -1;

      for (unsigned int j = 0; j < opts.size(); j++) { // Create plugin extension id -> output interface mapping.
         ifc_mapping[opts[j].ext_type] = opts[j].out_ifc_num;
         ifc = opts[j].out_ifc_num;
      }

      if (opts.size() == 0 || ifc < 0) {
         continue;
      }

      // Create unirec templates.
      template_str = tmp->get_unirec_field_string();
      if (tmp->include_basic_flow_fields()) {
         template_str += string(",") + BASIC_FLOW_TEMPLATE;
      } else {
         template_str += string (",") + PACKET_TEMPLATE;
      }

      tmplt[ifc] = ur_create_output_template(ifc, template_str.c_str(), &error);
      if (tmplt == NULL) {
         fprintf(stderr, "UnirecExporter: %s\n", error);
         free(error);
         free_unirec_resources();
         return -2;
      }
   }

   for (int i = 0; i < out_ifc_cnt; i++) { // Create unirec records.
      if (tmplt[i] != NULL) {
         record[i] = ur_create_record(tmplt[i], (i == basic_ifc_num ? 0 : 2048));

         if (record == NULL) {
            free_unirec_resources();
            return -3;
         }
      }
   }

   return 0;
}

/**
 * \brief Close connection and free resources.
 */
void UnirecExporter::close()
{
   for (int i = 0; i < out_ifc_cnt; i++) {
      trap_send(i, "", 1);
   }
   trap_finalize();

   free_unirec_resources();

   basic_ifc_num = -1;
   out_ifc_cnt = 0;
}

/**
 * \brief Free unirec templates and unirec records.
 */
void UnirecExporter::free_unirec_resources()
{
   if (tmplt) {
      for (int i = 0; i < out_ifc_cnt; i++) {
         if (tmplt[i] != NULL) {
            ur_free_template(tmplt[i]);
         }
      }
      delete [] tmplt;
      tmplt = NULL;
   }
   if (record) {
      for (int i = 0; i < out_ifc_cnt; i++) {
         if (record[i] != NULL) {
            ur_free_record(record[i]);
         }
      }
      delete [] record;
      record = NULL;
   }
}

int UnirecExporter::export_packet(Packet &pkt)
{
   RecordExt *ext = pkt.exts;
   vector<int> to_export; // Contains output ifc numbers.
   ur_template_t *tmplt_ptr = NULL;
   void *record_ptr = NULL;

   while (ext != NULL) {
      map<int, int>::iterator it = ifc_mapping.find(ext->extType); // Find if mapping exists.
      if (it != ifc_mapping.end()) {
         int ifc_num = it->second;
         if (ifc_num < 0) {
            ext = ext->next;
            continue;
         }

         tmplt_ptr = tmplt[ifc_num];
         record_ptr = record[ifc_num];

         if (find(to_export.begin(), to_export.end(), ifc_num) == to_export.end()) {
            to_export.push_back(ifc_num);
         }

         ur_clear_varlen(tmplt_ptr, record_ptr);
         memset(record_ptr, 0, ur_rec_fixlen_size(tmplt_ptr));
         fill_packet_fields(pkt, tmplt_ptr, record_ptr);
         ext->fillUnirec(tmplt_ptr, record_ptr); /* Add each extension header into unirec record. */
      }
      ext = ext->next;
   }

   for (unsigned int i = 0; i < to_export.size(); i++) {
      tmplt_ptr = tmplt[to_export[i]];
      record_ptr = record[to_export[i]];
      trap_send(to_export[i], record_ptr, ur_rec_fixlen_size(tmplt_ptr) + ur_rec_varlen_size(tmplt_ptr, record_ptr));
   }

   return 0;
}

int UnirecExporter::export_flow(FlowRecord &flow)
{
   RecordExt *ext = flow.exts;
   vector<int> to_export; // Contains output ifc numbers.
   ur_template_t *tmplt_ptr = NULL;
   void *record_ptr = NULL;

   if (basic_ifc_num >= 0) { // Process basic flow.
      tmplt_ptr = tmplt[basic_ifc_num];
      record_ptr = record[basic_ifc_num];

      ur_clear_varlen(tmplt_ptr, record_ptr);
      memset(record_ptr, 0, ur_rec_fixlen_size(tmplt_ptr));

      fill_basic_flow(flow, tmplt_ptr, record_ptr);
      to_export.push_back(basic_ifc_num);
   }

   while (ext != NULL) {
      map<int, int>::iterator it = ifc_mapping.find(ext->extType); // Find if mapping exists.
      if (it != ifc_mapping.end()) {
         int ifc_num = it->second;
         if (ifc_num < 0) {
            ext = ext->next;
            continue;
         }

         tmplt_ptr = tmplt[ifc_num];
         record_ptr = record[ifc_num];

         if (find(to_export.begin(), to_export.end(), ifc_num) == to_export.end()) {
            to_export.push_back(ifc_num);
         }

         ur_clear_varlen(tmplt_ptr, record_ptr);
         memset(record_ptr, 0, ur_rec_fixlen_size(tmplt_ptr));

         fill_basic_flow(flow, tmplt_ptr, record_ptr);
         ext->fillUnirec(tmplt_ptr, record_ptr); /* Add each extension header into unirec record. */
      }
      ext = ext->next;
   }

   for (unsigned int i = 0; i < to_export.size(); i++) {
      tmplt_ptr = tmplt[to_export[i]];
      record_ptr = record[to_export[i]];
      trap_send(to_export[i], record_ptr, ur_rec_fixlen_size(tmplt_ptr) + ur_rec_varlen_size(tmplt_ptr, record_ptr));
   }

   return 0;
}

/**
 * \brief Fill record with basic flow fields.
 * \param [in] flow Flow record.
 * \param [in] tmplt_ptr Pointer to unirec template.
 * \param [out] record_ptr Pointer to unirec record.
 */
void UnirecExporter::fill_basic_flow(FlowRecord &flow, ur_template_t *tmplt_ptr, void *record_ptr)
{
   ur_time_t tmp_time;

   if (flow.ip_version == 4) {
      ur_set(tmplt_ptr, record_ptr, F_SRC_IP, ip_from_4_bytes_be((char *) &flow.src_ip.v4));
      ur_set(tmplt_ptr, record_ptr, F_DST_IP, ip_from_4_bytes_be((char *) &flow.dst_ip.v4));
   } else {
      ur_set(tmplt_ptr, record_ptr, F_SRC_IP, ip_from_16_bytes_be((char *) flow.src_ip.v6));
      ur_set(tmplt_ptr, record_ptr, F_DST_IP, ip_from_16_bytes_be((char *) flow.dst_ip.v6));
   }

   tmp_time = ur_time_from_sec_msec(flow.start_timestamp.tv_sec, flow.start_timestamp.tv_usec / 1000.0);
   ur_set(tmplt_ptr, record_ptr, F_TIME_FIRST, tmp_time);

   tmp_time = ur_time_from_sec_msec(flow.end_timestamp.tv_sec, flow.end_timestamp.tv_usec / 1000.0);
   ur_set(tmplt_ptr, record_ptr, F_TIME_LAST, tmp_time);

   ur_set(tmplt_ptr, record_ptr, F_PROTOCOL, flow.ip_proto);
   ur_set(tmplt_ptr, record_ptr, F_SRC_PORT, flow.src_port);
   ur_set(tmplt_ptr, record_ptr, F_DST_PORT, flow.dst_port);
   ur_set(tmplt_ptr, record_ptr, F_PACKETS, flow.pkt_total_cnt);
   ur_set(tmplt_ptr, record_ptr, F_BYTES, flow.octet_total_length);
   ur_set(tmplt_ptr, record_ptr, F_TCP_FLAGS, flow.tcp_control_bits);
   ur_set(tmplt_ptr, record_ptr, F_TOS, flow.ip_tos);
   ur_set(tmplt_ptr, record_ptr, F_TTL, flow.ip_ttl);

   //ur_set(tmplt_ptr, record_ptr, F_DIR_BIT_FIELD, 0);
   //ur_set(tmplt_ptr, record_ptr, F_LINK_BIT_FIELD, 0);
}


/**
 * \brief Fill record with basic flow fields.
 * \param [in] flow Flow record.
 * \param [in] tmplt_ptr Pointer to unirec template.
 * \param [out] record_ptr Pointer to unirec record.
 */
void UnirecExporter::fill_packet_fields(Packet &pkt, ur_template_t *tmplt_ptr, void *record_ptr)
{
   ur_time_t tmp_time = ur_time_from_sec_msec(pkt.timestamp.tv_sec, pkt.timestamp.tv_usec / 1000.0);

   ur_set_var(tmplt_ptr, record_ptr, F_DST_MAC, pkt.packet, 6);
   ur_set_var(tmplt_ptr, record_ptr, F_SRC_MAC, pkt.packet + 6, 6);
   ur_set(tmplt_ptr, record_ptr, F_ETHERTYPE, pkt.ethertype);
   ur_set(tmplt_ptr, record_ptr, F_TIME, tmp_time);
}

