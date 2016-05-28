/**
 * This file is part of Low-Energy-Meter.
 *
 * Copyright 2016 Frank Dürr
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <bcm2835.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <signal.h>
#include <sched.h>
#include <sys/mman.h>
#include <stdbool.h>
#include "mcp320x.h"
#include "ring.h"

/* Default task priority */
#define DEFAULT_TASK_PRIORITY 49

/* Estimated maximum stack size */
#define MAX_STACK_SIZE (RING_SIZE*sizeof(struct ring_entry) + 1024) 

/* GPIO pins controlling charge and discharge relay */
const RPiGPIOPin charge_pin = RPI_GPIO_P1_18; 
const RPiGPIOPin discharge_pin = RPI_GPIO_P1_16; 

/* ADC channel measuring voltage of capacitor / device */
const enum channel_singleended adc_channel = CH0;

/* SPI settings. */
/* The Raspberry Pi has two SS pins called CE0 and CE1. 
   The board uses CE0 (BCM2835_SPI_CS0). */
const bcm2835SPIChipSelect spi_cs = BCM2835_SPI_CS0;
/* Since we operate the MCP3208 at 3.3 V, the SPI clock rate should be <= 1 MHz 
   (cf. MCP3208 datasheet). 500 kHz should be a safe setting. */
const unsigned int spi_frequency = 500000;

FILE *fout = NULL;

int task_priority;
struct timespec sampling_interval;
double sampling_frequency;

uint16_t threshold_upper;
uint16_t threshold_lower;

struct ring the_ring;

pthread_t sampling_thread;
pthread_t logger_thread;

bool is_spi_open = false;
bool is_bcm_open = false;

/**
 * Gracefully terminate the process.
 *
 * @param status process exit status
 */
void die(int status)
{
     if (fout != NULL)
	  fclose(fout);

     if (is_spi_open)
	  bcm2835_spi_end();

     if (is_bcm_open)
	  bcm2835_close();
     
     exit(status);
}

/**
 * SIGINT signal handler.
 */
void sig_int(int signo)
{
     pthread_cancel(sampling_thread);
     pthread_cancel(logger_thread);
}

/**
 * Print usage information.
 */
void usage(const char *appl)
{
     fprintf(stderr, "%s -f SAMPLING_FREQUENCY -l LOWER_THRESHOLD "
	     "-u UPPER_THRESHOLD -o LOGFILE [-p TASK_PRIORITY] \n", appl);
}

/**
 * Pref-fault stack memory for deterministic time to access stack memory.
 */
void stack_prefault(void)
{
     unsigned char dummy[MAX_STACK_SIZE];
     memset(dummy, 0, MAX_STACK_SIZE);
     return;
}

/**
 * Convert a frequency value to a time interval.
 *
 * @param frequency frequency in Hertz
 * @return time interval
 */
struct timespec frequency_to_interval(double frequency)
{
     struct timespec itimespec;
     
     /* Calculate interval in nanoseconds.
        A 64 bit value is enough for thousands of years sampling. */
     uint64_t ins = (uint64_t) (1000000000.0/frequency + 0.5);

     itimespec.tv_sec = ins/1000000000ull;
     itimespec.tv_nsec = ins%1000000000ull;

     return itimespec;
}

/**
 * Calculate timestamp of next sample.
 *
 * @param tlast time of last sample
 * @param interval sampling interval
 * @return time of next sample
 */
struct timespec next_sampling_time(struct timespec tlast,
				   struct timespec interval)
{
     struct timespec tnext;
     
     tnext.tv_sec = tlast.tv_sec+interval.tv_sec;
     tnext.tv_nsec = tlast.tv_nsec+interval.tv_nsec;

     /* Normalize */
     if (tnext.tv_nsec >= 1000000000l) {
	  tnext.tv_sec++;
	  tnext.tv_nsec -= 1000000000l;
     }

     return tnext;
}

/**
 * Convert timespec values (sec, ns) to 64 bit nanosecond value.
 *
 * @param t timespec values
 * @return value in nanoseconds corresponding to timespec values
 */
uint64_t to_nanosec(struct timespec t)
{
     uint64_t t_ns = 1000000000ull*t.tv_sec;
     t_ns += t.tv_nsec;

     return t_ns;
}

/**
 * Write timestamped samples as CSV to log file.
 *
 * Format: comma-separated values 
 * timestamp [nanoseconds], value 
 *
 * @param fout output file
 * @param sample sample value
 * @param tsample timestamp of sample
 */
void log_sample(FILE *fout, int16_t sample, uint64_t t, uint64_t epoch)
{
     fprintf(fout, "%llu,%llu,%d\n", t, epoch, sample);
}

/**
 * Main loop of sampling thread.
 */
void *sampling_thread_loop(void *args)
{
     /* Set high ("realtime") priority for sampling thread */

     struct sched_param schedparam;
     schedparam.sched_priority = task_priority;
     if (sched_setscheduler(0, SCHED_FIFO, &schedparam) == -1) {
	  perror("sched_setscheduler failed");
	  die(-1);
     }

     /* Start infinite loop of charging-discharging cycles until user 
	interrupts. */

     /* Start in charge state */ 
     struct timespec twait;
     enum State {charging, discharging} state = charging;
     // CAUTION: First open discharge relay before closing charge
     // relay! Otherwise, a high current might flow into to
     // discharged capacitor by-passing the limiting resistor.
     bcm2835_gpio_clr(discharge_pin);
     // Wait 100 ms to be sure that discharge relay is open
     // (relay should open within less than 10 ms
     // according to datasheet, so 100 ms should be safe).
     twait.tv_sec = 0;
     twait.tv_nsec = 100000000;
     clock_nanosleep(CLOCK_MONOTONIC, 0, &twait, NULL);
     bcm2835_gpio_set(charge_pin);

     uint64_t epoch = 0;
     struct timespec tsample;
     clock_gettime(CLOCK_MONOTONIC, &tsample);
     while (true) {
	  // Take a sample 
	  int16_t sample = get_sample_singleended(adc_channel);

	  // Timestamp sample
	  struct timespec tnow;
	  clock_gettime(CLOCK_MONOTONIC , &tnow);
	       
	  if (sample == -1) {
	       fprintf(stderr, "Error while taking sample\n");
	  } else if (state == charging) {
	       // Stop charging when upper threshold was passed and then
	       // switch to discharging phase.
	       if (sample >= threshold_upper) {
		    // Charged. Switch to discharging phase.
		    state = discharging;
		    // CAUTION: First open charge relay before closing discharge
		    // relay! Otherwise, a high current might flow into to
		    // capacitor by-passing the limiting resistor.
		    bcm2835_gpio_clr(charge_pin);
		    // Wait 100 ms to be sure that charge relay is open
		    // (relay should open within less than 10 ms
		    // according to datasheet, so 100 ms should be safe).
		    twait.tv_sec = 0;
		    twait.tv_nsec = 100000000;
		    clock_nanosleep(CLOCK_MONOTONIC, 0, &twait, NULL);
		    bcm2835_gpio_set(discharge_pin);
		    // New sampling period starts now (right before taking
		    // next sample).
		    epoch++;
		    clock_gettime(CLOCK_MONOTONIC, &tsample);
	       } else {
		    // Go on charging.
		    // Sleep until next sampling time
		    tsample = next_sampling_time(tsample, sampling_interval);
		    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tsample, 
				    NULL);
	       }
	  } else if (state == discharging) {
	       // Record sample.
	       struct ring_entry entry;
	       entry.timestamp = to_nanosec(tnow);
	       entry.value = sample;
	       entry.epoch = epoch;
	       ring_put(&the_ring, &entry);

	       // Switch to charging phase when lower threshold was passed.
	       if (sample <= threshold_lower) {		    
		    // Discharged. Switch to charging phase.
		    state = charging;
		    // CAUTION: First open discharge relay before closing 
		    // charge relay! Otherwise, a high current might flow into 
		    // to discharged capacitor by-passing the limiting resistor.
		    bcm2835_gpio_clr(discharge_pin);
		    // Wait 100 ms to be sure that discharge relay is open
		    // (relay should open within less than 10 ms
		    // according to datasheet, so 100 ms should be safe).
		    twait.tv_sec = 0;
		    twait.tv_nsec = 100000000;
		    clock_nanosleep(CLOCK_MONOTONIC, 0, &twait, NULL);
		    bcm2835_gpio_set(charge_pin);			 
	       } else {
		    // Go on discharging.
		    // Sleep until next sampling time
		    tsample = next_sampling_time(tsample, sampling_interval);
		    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tsample, 
				    NULL);
	       }
	  }
     }
}

/**
 * Main loop of logger thread.
 */
void *logger_thread_loop(void *args)
{
     /* Set high ("realtime") priority for sampling thread */

     struct sched_param schedparam;
     /* Give logger thread a lower priority than sampling thread, so on a 
        single core system it does not get into the way of the sampling
	thread. */ 
     schedparam.sched_priority = task_priority-1;
     if (sched_setscheduler(0, SCHED_FIFO, &schedparam) == -1) {
	  perror("sched_setscheduler failed");
	  die(-1);
     }
     
     while (true) {
	  struct ring_entry entry;
	  ring_get(&the_ring, &entry);
	  log_sample(fout, entry.value, entry.timestamp, entry.epoch);
     }
}

/* Configure GPIO pins controlling charge and discharge relays */
void setup_gpio()
{
     bcm2835_gpio_fsel(charge_pin, BCM2835_GPIO_FSEL_OUTP);
     bcm2835_gpio_clr(charge_pin);

     bcm2835_gpio_fsel(discharge_pin, BCM2835_GPIO_FSEL_OUTP);
     bcm2835_gpio_clr(discharge_pin);
}

/**
 * The main function.
 */
int main(int argc, char *argv[])
{
     /* Parse command line arguments */
     
     char *sampling_frequency_arg = NULL;
     char *logfile_arg = NULL;
     char *threshold_upper_arg = NULL;
     char *threshold_lower_arg = NULL;
     char *task_priority_arg = NULL;
     int c;
     while ((c = getopt(argc, argv, "f:o:p:l:u:")) != -1) {
	  switch (c) {
	  case 'f' :
	       sampling_frequency_arg = malloc(strlen(optarg)+1);
	       strcpy(sampling_frequency_arg, optarg);
	       break;
	  case 'o' :
	       logfile_arg = malloc(strlen(optarg)+1);
	       strcpy(logfile_arg, optarg);
	       break;
	  case 'l' :
	       threshold_lower_arg = malloc(strlen(optarg)+1);
	       strcpy(threshold_lower_arg, optarg);
	       break;
	  case 'u' :
	       threshold_upper_arg = malloc(strlen(optarg)+1);
	       strcpy(threshold_upper_arg, optarg);
	       break;
	  case 'p' :
	       task_priority_arg = malloc(strlen(optarg)+1);
	       strcpy(task_priority_arg, optarg);
	       break;
	  case '?':
	       fprintf(stderr, "Unknown option\n");
	       usage(argv[0]);
	       die(-1);
	       break;
	  }
     }
	      
     if (sampling_frequency_arg == NULL || logfile_arg == NULL ||
	 threshold_lower_arg == NULL || threshold_upper_arg == NULL) {
	  usage(argv[0]);
	  die(-1);
     }

     threshold_lower = atoi(threshold_lower_arg);
     threshold_upper = atoi(threshold_upper_arg);
     sampling_frequency = strtod(sampling_frequency_arg, NULL);
     sampling_interval = frequency_to_interval(sampling_frequency);

     if (task_priority_arg != NULL) {
	  task_priority = atoi(task_priority_arg);
     } else {
	  task_priority = DEFAULT_TASK_PRIORITY;
     }
     
     /* Setup SPI */

     if (bcm2835_init() == 0) {
	  die(-1);
     } else {
	  is_bcm_open = true;
     }
     
     bcm2835_spi_begin();
     is_spi_open = true;
     
     bcm2835_spi_chipSelect(spi_cs);

     // Set divider according to SPI frequency
     uint16_t divider = (uint16_t) ((double) 250000000/spi_frequency + 0.5);
     bcm2835_spi_setClockDivider(divider);

     // SPI 0,0 as per MCP3208 data sheet
     bcm2835_spi_setDataMode(BCM2835_SPI_MODE0);

     bcm2835_spi_setBitOrder(BCM2835_SPI_BIT_ORDER_MSBFIRST);

     bcm2835_spi_setChipSelectPolarity(BCM2835_SPI_CS0, LOW);

     // Configure GPIO pins as output and set output to low.
     setup_gpio();
     
     /* Open log file */

     fout = fopen(logfile_arg, "w");
     if (fout == NULL) {
	  perror("Could not open log file");
	  die(-1);
     }

     // Init ring buffer for communicate between sampling and logging threads.

     ring_init(&the_ring);

     /* Lock memory and prefault stack */

     if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
	  perror("mlockall failed");
	  die(-1);
     }

     stack_prefault();
     
     /* Create threads */
     
     if (pthread_create(&sampling_thread, NULL, sampling_thread_loop, NULL)) {
	  perror("Could not create sampling thread");
	  die(-1);
     }

     if (pthread_create(&logger_thread, NULL, logger_thread_loop, NULL)) {
	  perror("Could not create logger thread");
	  die(-1);
     }

     /* Install SIGINT signal handler for graceful termination */
     
     if (signal(SIGINT, sig_int) == SIG_ERR) {
	  perror("Could not set signal handler for SIGINT");
	  die(-1);
     }
     
     pthread_join(logger_thread, NULL);
     pthread_join(sampling_thread, NULL);

     die(0);
}
