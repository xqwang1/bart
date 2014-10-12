/* Copyright 2014. The Regents of the University of California.
 * All rights reserved. Use of this source code is governed by 
 * a BSD-style license which can be found in the LICENSE file.
 *
 * Authors: 
 * 2014 Martin Uecker <uecker@eecs.berkeley.edu>
 */

#include <getopt.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <complex.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>

#include "num/multind.h"

#include "misc/misc.h"
#include "misc/mri.h"
#include "misc/mmio.h"
#include "misc/debug.h"

#ifndef CFL_SIZE
#define CFL_SIZE sizeof(complex float)
#endif


/* Information about twix files can be found here:
 * (Matlab code by Philipp Ehses and others)
 * https://github.com/cjohnevans/Gannet2.0/blob/master/mapVBVD.m
 */ 
struct hdr_s {

	uint32_t offset;
	uint32_t nscans;
	uint32_t measid;
	uint32_t fileid;
	uint64_t datoff;
//	uint64_t length;
};

static void xread(int fd, void* buf, size_t size)
{
	if (size != (size_t)read(fd, buf, size))
		error("reading file");
}

static void xseek(int fd, off_t pos)
{
        if (-1 == lseek(fd, pos, SEEK_SET))
		error("seeking");
}

static bool siemens_meas_setup(int fd, struct hdr_s* hdr)
{
	off_t start = 0;

	xseek(fd, start);
	xread(fd, hdr, sizeof(struct hdr_s));

	// check for VD version
	bool vd = ((hdr->offset < 10000) && (hdr->nscans < 64));

	if (vd) {
	
		debug_printf(DP_INFO, "VD Header. MeasID: %d FileID: %d Scans: %d\n",
					hdr->measid, hdr->fileid, hdr->nscans);

		start += hdr->datoff;

		xseek(fd, start);

		// reread offset
		xread(fd, &hdr->offset, sizeof(hdr->offset));

	} else {

		debug_printf(DP_INFO, "VB Header.\n");
		hdr->nscans = 1;
	}

	start += hdr->offset;

	xseek(fd, start);

        return vd;
}


struct mdh2 {	// second part of mdh

	uint32_t evalinfo[2];
	uint16_t samples;
	uint16_t channels;
	uint16_t sLC[14];
	uint16_t dummy1[2];
	uint16_t clmnctr;
	uint16_t dummy2[5];
	uint16_t linectr;
	uint16_t partctr;
};



static void siemens_adc_read(bool vd, int fd, const long dims[DIMS], long pos[DIMS], complex float* buf)
{
	char scan_hdr[vd ? 192 : 0];
	xread(fd, scan_hdr, sizeof(scan_hdr));

	for (pos[COIL_DIM] = 0; pos[COIL_DIM] < dims[COIL_DIM]; pos[COIL_DIM]++) {

		char chan_hdr[vd ? 32 : 128];
		xread(fd, chan_hdr, sizeof(chan_hdr));

		struct mdh2 mdh;
		memcpy(&mdh, vd ? (scan_hdr + 40) : (chan_hdr + 20), sizeof(mdh));

		if (0 == pos[COIL_DIM]) {

			// TODO: rethink this
			pos[PHS1_DIM] = mdh.sLC[0]; // - mdh.linectr;
			pos[SLICE_DIM] = mdh.sLC[2];
			pos[PHS2_DIM] = mdh.sLC[3]; // - mdh.partctr;
			pos[TE_DIM] = mdh.sLC[4];
			pos[TIME_DIM] = mdh.sLC[6];
			pos[TIME2_DIM] = mdh.sLC[7];
		}

		debug_print_dims(DP_DEBUG1, DIMS, pos);

		if (dims[READ_DIM] != mdh.samples)
			error("wrong number of samples");

		assert(md_is_index(DIMS, pos, dims));

		xread(fd, buf + pos[COIL_DIM] * dims[READ_DIM], dims[READ_DIM] * CFL_SIZE);
	}

	pos[COIL_DIM] = 0;
}




static void usage(const char* name, FILE* fd)
{
	fprintf(fd, "Usage: %s [...] [-a A] <dat file> <output>\n", name);
}


static void help(void)
{
	printf( "\n"
		"Read data from Siemens twix (.dat) files.\n"
		"\n"
		"-x X\tnumber of samples (read-out)\n"
		"-y Y\tphase encoding steps\n"
		"-z Z\tpartition encoding steps\n"
		"-s S\tnumber of slices\n"
		"-c C\tnumber of channels\n"
		"-a A\ttotal number of ADCs\n"
		"-h\thelp\n");
}



int main(int argc, char* argv[argc])
{
	int c;
	long adcs = 0;

	long dims[DIMS];
	md_singleton_dims(DIMS, dims);

	while (-1 != (c = getopt(argc, argv, "x:y:z:s:c:a:h"))) {
		switch (c) {

		case 'x':
			dims[READ_DIM] = atoi(optarg);
			break;

		case 'y':
			dims[PHS1_DIM] = atoi(optarg);
			break;

		case 'z':
			dims[PHS2_DIM] = atoi(optarg);
			break;

		case 's':
			dims[SLICE_DIM] = atoi(optarg);
			break;

		case 'a':
			adcs = atoi(optarg);
			break;

		case 'c':
			dims[COIL_DIM] = atoi(optarg);
			break;

		case 'h':
			usage(argv[0], stdout);
			help();
			exit(0);

		default:
			usage(argv[0], stderr);
			exit(1);
		}
	}

        if (argc - optind != 2) {

		usage(argv[0], stderr);
		exit(1);
	}

	if (0 == adcs)
		adcs = dims[PHS1_DIM] * dims[PHS2_DIM] * dims[SLICE_DIM];

	debug_print_dims(DP_DEBUG1, DIMS, dims);

        int ifd;
        if (-1 == (ifd = open(argv[optind + 0], O_RDONLY)))
                error("error opening file.");

	struct hdr_s hdr;
	bool vd = siemens_meas_setup(ifd, &hdr);

	complex float* out = create_cfl(argv[optind + 1], DIMS, dims);

	long adc_dims[DIMS];
	md_select_dims(DIMS, READ_FLAG|COIL_FLAG, adc_dims, dims);

	void* buf = md_alloc(DIMS, adc_dims, CFL_SIZE);

	while (adcs--) {

		long pos[DIMS] = { [0 ... DIMS - 1] = 0 };
		siemens_adc_read(vd, ifd, dims, pos, buf);

		debug_print_dims(DP_DEBUG1, DIMS, pos);
		md_copy_block(DIMS, pos, dims, out, adc_dims, buf, CFL_SIZE); 
	}


	md_free(buf);
	unmap_cfl(DIMS, dims, out);
	exit(0);
}
