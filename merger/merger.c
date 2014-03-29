/**
 * \file merger.c
 * \brief Merge traffic incoming on mutiple interfaces.
 * \author Pavel Krobot <xkrobo01@cesnet.cz>
 * \date 2014
 */
/*
 * Copyright (C) 2013 CESNET
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
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <omp.h>

#include <libtrap/trap.h>
#include <unirec/unirec.h>
#include <buffer.h>


#define TS_LAST 	0
#define TS_FIRST	1
#define DEFAULT_TIMEOUT			10
#define DEFAULT_BUFFER_SIZE	20

#define MODE_TIME_IGNORE	0
#define MODE_TIME_AWARE		1



// Struct with information about module
trap_module_info_t module_info = {
   "Traffic Merger (2nd version)", // Module name
   // Module description
   "This module merges traffic from multiple input interfaces to one output\n"
   "interface. There are two supported versions:\n"
   "   - normal (default) - resending incoming data as they come.\n"
   "   - timestamp aware - incoming data are sended with respect to timestamp.\n"
   "     order.\n"
   "\n"
   "Interfaces:\n"
   "   Inputs: variable\n"
   "   Outputs: 1\n"
   "\n"
   "Usage:\n"
   "   ./merger -i IFC_SPEC -n CNT [-u IN_FMT] [-o OUT_FMT] [-T] [-F] [-s SIZE] [-t MS]\n"
   "\n"
   "Module specific parameters:\n"
   "   UNIREC_FMT   The i-th parameter of this type specifies format of UniRec\n"
   "                expected on the i-th input interface.\n"
   "   -F         (timestamp aware version) Sorts timestamps based on TIME_FIRST\n"
   "              field, instead of TIME_LAST (default).\n"
   "   -n CNT     Sets count of input links. Must correspond to parameter -i (trap).\n"
   "   -o OUT_FMT Set of fields included in the output (UniRec specifier).\n"
   "              (default <COLLECTOR_FLOW>).\n"
   "   -u IN_FMT  UniRec specifier of input data (same to all links).\n"
   "              (default <COLLECTOR_FLOW>).\n"
   "   -s SIZE    (timestamp aware version) Set size of buffer for incoming records.\n"
   "   -t MS      (timestamp aware version) Set initial timeout for incoming\n"
   "              interfaces (in miliseconds). Timeout is set to 0, if no data\n"
   "              received in initial timeout.\n"
   "   -T         Set mode to timestamp aware.\n",
   -1, // Number of input interfaces (-1 means variable)
   1, // Number of output interfaces
};

static int stop = 0;
static int verbose;

TRAP_DEFAULT_SIGNAL_HANDLER(stop = 1);


static int timestamp_selector = TS_LAST; // Tells to sort timestamps based on TIME_FIRST or TIME_LAST field

static ur_template_t *in_template; // UniRec template of input interface(s)
static ur_template_t *out_template; // UniRec template of output interface

static int n_threads=0; // Number of threads: one for every input inreface + one sender
static int initial_timeout = DEFAULT_TIMEOUT; // Initial timeout for incoming interfaces (in miliseconds)
static int buffer_size = 30 * sizeof(int);
static buff_t buffer;

static int *rcv_flag_field; // Flag field for input interfaces



void ta_capture_thread(int index)
{
	srand (time(NULL));

	int ret;
	int *result;

   if (index == 0) {//sending thread
		printf("Thread %i started as SENDER >>.\n", index);

		get_min_rec(&buffer, result);
   } else {// reading threads
		printf("Thread %i started as << READER.\n", index);
		int *rec;
		int rec_size = sizeof(int) * (index + 1);

		for (int i = 0; i < 10; ++i){
			rec = (int *)malloc(rec_size);
			if (rec == NULL){
				fprintf(stderr,"MALLOC ERROR - TODO\n");
				return;
				///!!!TODO ----------------------------------------------------------------- ERROR
			}
			rec[0] = index * 100 + i;
			for (int j = 1; j < (index + 1); ++j){
				rec[j] = rand() % 10 + 1;
			}

			#pragma omp critical
				ret = add_rec(&buffer, rec, rec_size);

			if (ret != EOK){
				printf("Thread %i exiting (%i records added).\n", index, i);
				break;
			}
		}
//		free(rec);
   }
}

void capture_thread(int index)
{
}


int main(int argc, char **argv)
{
   int ret;

   char *in_template_str = "<COLLECTOR_FLOW>";
   char *out_template_str = "<COLLECTOR_FLOW>";

   int mode = MODE_TIME_IGNORE;
   int n_inputs = 0;

   // ***** Process parameters *****
	// Let TRAP library parse command-line arguments and extract its parameters
   trap_ifc_spec_t ifc_spec;
   ret = trap_parse_params(&argc, argv, &ifc_spec);
   if (ret != TRAP_E_OK) {
      if (ret == TRAP_E_HELP) { // "-h" was found
         trap_print_help(&module_info);
         return 0;
      }
      fprintf(stderr, "ERROR in parsing of parameters for TRAP: %s\n", trap_last_error_msg);
      return 1;
   }

   verbose = trap_get_verbose_level();

   if (verbose >= 0){
      printf("Verbosity level: %i\n", trap_get_verbose_level());
   }

   // Parse remaining parameters and get configuration
   char opt;
   while ((opt = getopt(argc, argv, "b:Fn:9u:t:T")) != -1) {
      switch (opt) {
         case 'b':
				buffer_size = atoi(optarg);
				break;
         case 'F':
            timestamp_selector = TS_FIRST;
            break;
			case 'n':
            n_inputs = atoi(optarg);
            break;
			case 'u':
				out_template_str = optarg
            in_template_str = optarg;
            break;
			case 't':
            initial_timeout = atoi(optarg);
            break;
			case 'T':
				mode=MODE_TIME_AWARE;
				break;
         default:
            fprintf(stderr, "Error: Invalid arguments.\n");
            return 1;
      }
   }

	if (n_inputs == 0){
		fprintf(stderr, "Error: Missing number of input links (parameter -n CNT).\n");
		ret = -1;
		goto exit;
	} else if (n_inputs > 32) {
      fprintf(stderr, "Error: More than 32 interfaces is not allowed by TRAP library.\n");
      ret = -1;
		goto exit;
   }

   if (verbose >= 0) {
      printf("Number of inputs: %i\n", n_inputs);

      printf("Creating UniRec templates ...\n");
   }

   // Create input and output UniRec template
	in_template = ur_create_template(in_template_str);
	out_template = ur_create_template(out_template_str);
	if (in_template == NULL || out_template == NULL) {
		fprintf(stderr, "Error: Invalid template: %s\n", in_template_str);
		ret = -2;
		goto exit;
	}

   // ***** TRAP initialization *****
   // Set number of input interfaces
   module_info.num_ifc_in = n_inputs;

   if (verbose >= 0) {
      printf("Initializing TRAP library ...\n");
   }

   // Initialize TRAP library (create and init all interfaces)
   ret = trap_init(&module_info, ifc_spec);
   if (ret != TRAP_E_OK) {
      fprintf(stderr, "ERROR in TRAP initialization: %s\n", trap_last_error_msg);
      trap_free_ifc_spec(ifc_spec);
      ret = -3;
      goto exit;
   }

   // We don't need ifc_spec anymore, destroy it
   trap_free_ifc_spec(ifc_spec);

   // Register signal handler.
   TRAP_REGISTER_DEFAULT_SIGNAL_HANDLER();

   trap_ifcctl(TRAPIFC_OUTPUT, 0, TRAPCTL_SETTIMEOUT, TRAP_NO_WAIT);

   if (verbose >= 0) {
      printf("Initialization done.\n");
   }


	if (mode == MODE_TIME_AWARE){
		n_threads = n_inputs + 1;

		rcv_flag_field = (int *) malloc(n_inputs * sizeof(int));

		buff_init(&buffer, buffer_size);
	} else {
		n_threads = n_inputs;
	}

	 // ***** Start a thread for each interface *****
   #pragma omp parallel num_threads(n_threads)
   {
   	if (mode == MODE_TIME_AWARE)
			ta_capture_thread(omp_get_thread_num());
		else
			capture_thread(omp_get_thread_num());
   }

	buff_item_t *it = buffer.first;
   for (int i = 0; i < buffer.item_count; ++i){
//   	int *rec = it->data;
//		printf("%i. %i: %i\n", i, rec[0], rec[1]);
		printf("%i. ", i);
		send_item(it);
		printf("\n");
		it = it->next_item;
   }

   ret = 0;

   // ***** Cleanup *****
   if (verbose >= 0) {
      printf("Exitting ...\n");
   }

   trap_terminate(); // This have to be called before trap_finalize(), otherwise it may crash (don't know if feature or bug in TRAP)

   ur_free_template(in_template);
   ur_free_template(out_template);

	if (mode == MODE_TIME_AWARE){
		free(rcv_flag_field);
	}

exit:
   // Do all necessary cleanup before exiting
   TRAP_DEFAULT_FINALIZATION();

   return ret;
}
// END OF merger.c