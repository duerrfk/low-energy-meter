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

#include "mcp320x.h"

#include <bcm2835.h>

const uint8_t startbit = 0x80;
const uint8_t sebit = 0x40;

/**
 * Request a sample from MCP320x.
 * 
 * @param channel_config the 3 channel configuration bits as defined
 * in the MCP3204/3208 data sheet.
 * @return 12 bit sample value, or -1 in case of an error.
 */
int16_t sample(uint8_t channel_config)
{
     uint8_t config = startbit | sebit | (channel_config << 3);

     uint16_t s;
     uint8_t data[3];
     data[0] = config;
    
     /* This function will overwrite the given data with the received data. */
     bcm2835_spi_transfern((char *) data, 3);

     /* 12 bit sample is returned most-significant bit first, 7 bit
	times after sending the config bits. */
     s = ((uint16_t) (data[0]&0x01)) << 11;
     s |= (uint16_t) (data[1]) << 3;
     s |= (uint16_t) ((data[2]&0xe0) >> 5);

     return ((int16_t) s);
}

int16_t get_sample_singleended(enum channel_singleended adc_channel)
{
     uint8_t channel;
     switch (adc_channel) {
     case CH0:
	  channel = 0;
	  break;
     case CH1:
	  channel = 1;
	  break;
     case CH2:
	  channel = 2;
	  break;
     case CH3:
	  channel = 3;
	  break;
     case CH4:
	  channel = 4;
	  break;
     case CH5:
	  channel = 5;
	  break;
     case CH6:
	  channel = 6;
	  break;
     case CH7:
	  channel = 7;
	  break;
     default:
	  return -1;
     }

     return sample(channel);
}

int16_t get_sample_diff(enum channel_differential adc_channel)
{
     uint8_t channel;
     switch (adc_channel) {
     case CH0CH1 :
	  channel = 0;
	  break;
     case CH1CH0 :
	  channel = 1;
	  break;
     case CH2CH3 :
	  channel = 2;
	  break;
     case CH3CH2 :
	  channel = 3;
	  break;
     case CH4CH5 :
	  channel = 4;
	  break;
     case CH5CH4 :
	  channel = 5;
	  break;
     case CH6CH7 :
	  channel = 6;
	  break;
     case CH7CH6 :
	  channel = 7;
	  break;
     default:
	  return -1;
     }

     return sample(channel);
}
