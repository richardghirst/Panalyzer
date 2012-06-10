/*
 * Panalyzer.  A Logic Analyzer for the RaspberryPi
 * Copyright (c) 2012 Richard Hirst <richardghirst@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef PANALYZER_H_
#define PANALYZER_H_

#define MAX_TRIGGERS	4
#define PAN_MAGIC		0x50414E41
#define PAN_VERSION		1

struct panctl_s {
	uint32_t	magic;
	uint32_t	version;
	uint32_t	timestamp;
	uint32_t	channel_mask;
	uint32_t	sample_rate;
	uint32_t	num_samples;
	uint32_t	trigger_point;
	struct {
		uint32_t	enabled;
		uint32_t	mask;
		uint32_t	value;
		uint32_t	min_samples;
	} trigger[MAX_TRIGGERS];
};
typedef struct panctl_s panctl_t;
typedef panctl_t *panctl_p;

#define MAX_CHANNELS	8

#define DEF_CHANNELS	{ 4,17,18,21 }
//#define DEF_CHANNELS	{ 5,4,3,2,1,0 }

#define DEF_PANCTL \
	{ \
		.magic			= PAN_MAGIC, \
		.version		= PAN_VERSION, \
		.channel_mask	= 1<<4|1<<17|1<<18|1<<21, \
		.sample_rate	= 1, \
		.num_samples	= 10000, \
		.trigger_point	= 0, \
	}

#endif /* PANALYZER_H_ */
