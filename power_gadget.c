/*
Copyright (c) 2012, Intel Corporation

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Written by Martin Dimitrov, Carl Strickland */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>

#include "rapl.h"

char         *progname;
const char   *version = "2.2";
uint64_t      num_node = 0;
uint64_t      delay_us = 1000000;
double        duration = 3600.0;
double        delay_unit = 1000000.0;

enum fields {
    systime =      0x1 << 0,
    rdtsc =        0x1 << 1,
    elapsed =      0x1 << 2,
    ia_freq =      0x1 << 3,
    powerW =       0x1 << 4,
    energyJ =      0x1 << 5,
    energyW =      0x1 << 6,
    ia_powerW =    0x1 << 7,
    ia_energyJ =   0x1 << 8,
    ia_energyW =   0x1 << 9,
    gt_powerW =    0x1 << 10,
    gt_energyJ =   0x1 << 11,
    gt_energyW =   0x1 << 12,
    dram_powerW =  0x01 << 13,
    dram_energyJ = 0x01 << 14,
    dram_energyW = 0x01 << 15,

    ENUM_SIZE = 16
};

uint64_t fields_to_render = 0;


double
get_rapl_energy_info(uint64_t power_domain, uint64_t node)
{
    int          err;
    double       total_energy_consumed;

    switch (power_domain) {
    case PKG:
        err = get_pkg_total_energy_consumed(node, &total_energy_consumed);
        break;
    case PP0:
        err = get_pp0_total_energy_consumed(node, &total_energy_consumed);
        break;
    case PP1:
        err = get_pp1_total_energy_consumed(node, &total_energy_consumed);
        break;
    case DRAM:
        err = get_dram_total_energy_consumed(node, &total_energy_consumed);
        break;
    default:
        err = MY_ERROR;
        break;
    }

    return total_energy_consumed;
}

void
convert_time_to_string(struct timeval tv, char* time_buf)
{
    time_t sec;
    int msec;
    struct tm *timeinfo;
    char tmp_buf[9];

    sec = tv.tv_sec;
    timeinfo = localtime(&sec);
    msec = tv.tv_usec/1000;

    strftime(tmp_buf, 9, "%H:%M:%S", timeinfo);
    sprintf(time_buf, "%s:%d",tmp_buf,msec);
}

double
convert_time_to_sec(struct timeval tv)
{
    double elapsed_time = (double)(tv.tv_sec) + ((double)(tv.tv_usec)/1000000);
    return elapsed_time;
}

typedef
struct header_support_pair_s {
    char* name;
    int hsd;
} header_support_pair;

header_support_pair column_headers[ENUM_SIZE] = {
    {.name="System Time", .hsd=-1},
    {.name="RDTSC", .hsd=-1},
    {.name="Elapsed Time", .hsd=-1},
    {.name="IA Frequency (%d)", .hsd=RAPL_PP0},

    {.name="Processor Power (%d) (Watts)", .hsd=RAPL_PKG},
    {.name="Cumulative Processor Energy (%d) (Joules)", .hsd=RAPL_PKG},
    {.name="Cumulative Processor Energy (%d) (mWh)", .hsd=RAPL_PKG},

    {.name="IA Power (%d) (Watts)", .hsd=RAPL_PP0},
    {.name="Cumulative IA Energy (%d) (Joules)", .hsd=RAPL_PP0},
    {.name="Cumulative IA Energy (%d) (mWh)", .hsd=RAPL_PP0},

    {.name="GT Power (%d) (Watts)", .hsd=RAPL_PP1},
    {.name="Cumulative GT Energy (%d) (Joules)", .hsd=RAPL_PP1},
    {.name="Cumulative GT Energy (%d) (mWh)", .hsd=RAPL_PP1},

    {.name="DRAM Power (%d) (Watts)", .hsd=RAPL_DRAM},
    {.name="Cumulative DRAM Energy (%d) (Joules)", .hsd=RAPL_DRAM},
    {.name="Cumulative DRAM Energy (%d) (mWh)", .hsd=RAPL_DRAM},
};

int index_from_field(int field) {
    for (int i=0; i<ENUM_SIZE; i++)
        if (0x1<<i == field)
            return i;
    return -1;
}


void
do_print_energy_info()
{
    int i = 0;
    int domain = 0;
    uint64_t node = 0;
    double new_sample;
    double delta;
    double power;

    double prev_sample[num_node][RAPL_NR_DOMAIN];
    double power_watt[num_node][RAPL_NR_DOMAIN];
    double cum_energy_J[num_node][RAPL_NR_DOMAIN];
    double cum_energy_mWh[num_node][RAPL_NR_DOMAIN];

    char time_buffer[32];
    struct timeval tv;
    int msec;
    uint64_t tsc;
    uint64_t freq;
    double start, end, interval_start;
    double total_elapsed_time;
    double interval_elapsed_time;

    /* don't buffer if piped */
    setbuf(stdout, NULL);

    /* Print header */
    int print_comma = 0;
    #define COMMA() if (print_comma) fprintf(stdout, ", "); print_comma = 1;

    printf("%x\n", fields_to_render);
    for (i = node; i < num_node; i++) {
        for (int ent=0; ent<=ENUM_SIZE; ent++) {
            if (!(0x1<<ent & fields_to_render))
                continue;
            if (column_headers[ent].hsd == -1) {
                if (node == i) {
                    COMMA();
                    fprintf(stdout, column_headers[ent].name);
                }
            } else {
                if (is_supported_domain(column_headers[ent].hsd)) {
                    COMMA();
                    fprintf(stdout, column_headers[ent].name, i);
                }
            }
        } 
    }

    fprintf(stdout, "\n");

    /* Read initial values */
    for (i = node; i < num_node; i++) {
        for (domain = 0; domain < RAPL_NR_DOMAIN; ++domain) {
            if(is_supported_domain(domain)) {
                prev_sample[i][domain] = get_rapl_energy_info(domain, i);
            }
        }
    }

    gettimeofday(&tv, NULL);
    start = convert_time_to_sec(tv);
    end = start;

    /* Begin sampling */
    while (1) {

        usleep(delay_us);

        gettimeofday(&tv, NULL);
        interval_start = convert_time_to_sec(tv);
        interval_elapsed_time = interval_start - end;

        for (i = node; i < num_node; i++) {
            for (domain = 0; domain < RAPL_NR_DOMAIN; ++domain) {
                if(is_supported_domain(domain)) {
                    new_sample = get_rapl_energy_info(domain, i);
                    delta = new_sample - prev_sample[i][domain];

                    /* Handle wraparound */
                    if (delta < 0) {
                        delta += GetMaxEnergyStatusJoules();
                    }

                    prev_sample[i][domain] = new_sample;

                    // Use the computed elapsed time between samples (and not
                    // just the sleep delay, in order to more accourately account for
                    // the delay between samples
                    power_watt[i][domain] = delta / interval_elapsed_time;
                    cum_energy_J[i][domain] += delta;
                    cum_energy_mWh[i][domain] = cum_energy_J[i][domain] / 3.6; // mWh
                }
            }
        }

        gettimeofday(&tv, NULL);
        end = convert_time_to_sec(tv);
        total_elapsed_time = end - start;
        convert_time_to_string(tv, time_buffer);

        read_tsc(&tsc);


        print_comma = 0;
        if (0x1<<0 & fields_to_render) {
            COMMA();
            fprintf(stdout, "%s", time_buffer);
        }

        if (0x1<<1 & fields_to_render) {
            COMMA();
            fprintf(stdout, "%lu", tsc);
        }
        
        if (0x1<<2 & fields_to_render) {
            COMMA();
            fprintf(stdout, "%.4lf", total_elapsed_time);
        }

        for (i = node; i < num_node; i++) {
            for (domain = 0; domain < RAPL_NR_DOMAIN; ++domain) {
                if (0x1<<3 & fields_to_render) {
                    get_pp0_freq_mhz(i, &freq);
                    COMMA();
                    fprintf(stdout, "%lu", freq);
                }

                if (!is_supported_domain(domain))
                    continue;

                int pow = domain * 3 + 4;
                int eng = domain * 3 + 5;
                int wat = domain * 3 + 6;

                if (0x1 << pow & fields_to_render) {
                    COMMA();
                    fprintf(stdout, "%.4lf", power_watt[i][domain]);
                }

                if (0x1 << eng & fields_to_render) {
                    COMMA();
                    fprintf(stdout, "%.4lf", cum_energy_J[i][domain]);
                }

                if (0x1 << wat & fields_to_render) {
                    COMMA();
                    fprintf(stdout, "%.4lf", cum_energy_mWh[i][domain]);
                }
            }
        }
        fprintf(stdout, "\n");

        // check to see if we are done
        if(total_elapsed_time >= duration)
            break;
    }

    end = clock();

    /* Print summary */
    fprintf(stdout, "\nTotal Elapsed Time(sec)=%.4lf\n\n", total_elapsed_time);
    for (i = node; i < num_node; i++) {
        if(is_supported_domain(RAPL_PKG)){
            fprintf(stdout, "Total Processor Energy_%d(Joules)=%.4lf\n", i, cum_energy_J[i][RAPL_PKG]);
            fprintf(stdout, "Total Processor Energy_%d(mWh)=%.4lf\n", i, cum_energy_mWh[i][RAPL_PKG]);
            fprintf(stdout, "Average Processor Power_%d(Watt)=%.4lf\n\n", i, cum_energy_J[i][RAPL_PKG]/total_elapsed_time);
        }
        if(is_supported_domain(RAPL_PP0)){
            fprintf(stdout, "Total IA Energy_%d(Joules)=%.4lf\n", i, cum_energy_J[i][RAPL_PP0]);
            fprintf(stdout, "Total IA Energy_%d(mWh)=%.4lf\n", i, cum_energy_mWh[i][RAPL_PP0]);
            fprintf(stdout, "Average IA Power_%d(Watt)=%.4lf\n\n", i, cum_energy_J[i][RAPL_PP0]/total_elapsed_time);
        }
        if(is_supported_domain(RAPL_PP1)){
            fprintf(stdout, "Total GT Energy_%d(Joules)=%.4lf\n", i, cum_energy_J[i][RAPL_PP1]);
            fprintf(stdout, "Total GT Energy_%d(mWh)=%.4lf\n", i, cum_energy_mWh[i][RAPL_PP1]);
            fprintf(stdout, "Average GT Power_%d(Watt)=%.4lf\n\n", i, cum_energy_J[i][RAPL_PP1]/total_elapsed_time);
        }
        if(is_supported_domain(RAPL_DRAM)){
            fprintf(stdout, "Total DRAM Energy_%d(Joules)=%.4lf\n", i, cum_energy_J[i][RAPL_DRAM]);
            fprintf(stdout, "Total DRAM Energy_%d(mWh)=%.4lf\n", i, cum_energy_mWh[i][RAPL_DRAM]);
            fprintf(stdout, "Average DRAM Power_%d(Watt)=%.4lf\n\n", i, cum_energy_J[i][RAPL_DRAM]/total_elapsed_time);
        }
    }
    read_tsc(&tsc);
    fprintf(stdout,"TSC=%lu\n", tsc);
}

void
usage()
{
    fprintf(stdout, "\nIntel(r) Power Gadget %s\n", version);
    fprintf(stdout, "\nUsage: \n");
    fprintf(stdout, "%s [-e [sampling delay (ms) ] optional] -d [duration (sec)]\n", progname);
    fprintf(stdout, "\nExample: %s -e 1000 -d 10\n", progname);
    fprintf(stdout, "\n");
}


void show_fopts() {
    fprintf(stdout, "\"-f\" options (comma separated):\n");
    #define APPLY(ENUM) printf("\t%s: %s\n", #ENUM, column_headers[index_from_field(ENUM)].name);
    APPLY(systime);
    APPLY(rdtsc);
    APPLY(elapsed);
    APPLY(ia_freq);
    APPLY(powerW);
    APPLY(energyJ);
    APPLY(energyW);
    APPLY(ia_powerW);
    APPLY(ia_energyJ);
    APPLY(ia_energyW);
    APPLY(gt_powerW);
    APPLY(gt_energyJ);
    APPLY(gt_energyW);
    APPLY(dram_powerW);
    APPLY(dram_energyJ);
    APPLY(dram_energyW);
    #undef APPLY
}

int
cmdline(int argc, char **argv)
{
    int             opt;
    uint64_t    delay_ms_temp = 1000;

    progname = argv[0];

    while ((opt = getopt(argc, argv, "e:d:f:F")) != -1) {
        switch (opt) {
        case 'e':
            delay_ms_temp = atoi(optarg);
            if(delay_ms_temp >= 50) {
                delay_us = delay_ms_temp * 1000;
            } else {
                fprintf(stdout, "Sampling delay must be greater than 50 ms.\n");
                return -1;
            }
            break;
        case 'd':
            duration = atof(optarg);
            if(duration <= 0.0){
                fprintf(stdout, "Duration must be greater than 0 seconds.\n");
                return -1;
            }
            break;
        case 'h':
            usage();
            exit(0);
            break;
        case 'f':
            char *cop = strdup(optarg);
            char *each = strtok(cop, ",");
            while (each != NULL) {
                #define APPLY(ENUM) if (chk && strcmp(each, #ENUM) == 0) {chk = 0; fields_to_render |= ENUM;}
                int chk = 1;
                APPLY(systime);
                APPLY(rdtsc);
                APPLY(elapsed);
                APPLY(ia_freq);
                APPLY(powerW);
                APPLY(energyJ);
                APPLY(energyW);
                APPLY(ia_powerW);
                APPLY(ia_energyJ);
                APPLY(ia_energyW);
                APPLY(gt_powerW);
                APPLY(gt_energyJ);
                APPLY(gt_energyW);
                APPLY(dram_powerW);
                APPLY(dram_energyJ);
                APPLY(dram_energyW);
                #undef APPLY
                if (chk) {
                    printf("unknown field: '%s'\n", each);
                    show_fopts();
                    return -1;
                }
                each = strtok(NULL, ",");
            }
            free(cop);
            break;
        case 'F':
            show_fopts();
        default:
            usage();
            return -1;
        }
    }
    return 0;
}

void sigint_handler(int signum)
{
    terminate_rapl();
    exit(0);
}

int
main(int argc, char **argv)
{
    int i = 0;
    int ret = 0;

    /* Clean up if we're told to exit */
    signal(SIGINT, sigint_handler);

    if (argc < 2) {
        usage();
        terminate_rapl();
        return 0;
    }

    // First init the RAPL library
    if (0 != init_rapl()) {
        fprintf(stdout, "Init failed!\n");
	terminate_rapl();
        return MY_ERROR;
    }
    num_node = get_num_rapl_nodes_pkg();

    ret = cmdline(argc, argv);
    if (ret) {
        terminate_rapl();
        return ret;
    }

    do_print_energy_info();

    terminate_rapl();
}
