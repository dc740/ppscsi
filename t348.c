/*
	t348.c	(c) 1997-1999 Grant Guenther <grant@torque.net>

	This is the low-level protocol module for the Adaptec APA-348
	(aka Trantor T348) parallel port SCSI adapter.  It forms part
	of the 'ppSCSI' suite of drivers.

*/

#define T348_VERSION	"0.92"

#define PPSC_BASE
#define PPSC_HA_MODULE

#include "ppscsi.h"

#define j44(a,b)                (((a<<1)&0xf0)+((b>>3)&0x0f))

static char t348_map[256];	/* status bits permutation */

static void t348_init (PHA *pha)
{

/*			 { REQ, BSY, MSG,  CD,	IO}	*/

	char key[5] = {0x20,0x40,0x10,0x08,0x04};

	ppsc_make_map(t348_map,key,0);
	sprintf(pha->ident,"t348 %s (%s), Adaptec APA-348",
		T348_VERSION,PPSC_H_VERSION);
}

static void t348_write_regr (PHA *pha, int regr, int value)
{
       w0(0x40+regr); w2(1); w2(0); w0(value); w2(8); w2(0);
}

static int t348_read_regr (PHA *pha, int regr)
{
	int s,a,b;

	w0(0x10+regr); s = r2(); w2(s|1); w2(s); w2(8);
	w0(0x80); a = r1(); w0(0); b = r1(); w2(0);
	return j44(a,b);
}

static void t348_connect (PHA *pha)
{
	int t;

	pha->saved_r0 = r0();
	w0(0);
	t = r2();
	w2(t%16); w0(0xfe); w2(t%4); w2((t%4)+8); w2(0);
	pha->saved_r2 = t;
}

static void t348_disconnect (PHA *pha)
{
	w0(0x71); w2(1); w2(0);
	w0(pha->saved_r0);
	w2(pha->saved_r2);
}

static int t348_test_proto (PHA *pha)
{
	int k, e, a, b;
	int wnt[3] = {0x6c, 0x55, 0xaa};

	e = 0;

	t348_connect(pha);

	switch (pha->mode) {

	case 0:	w0(0x70); w2(1); w2(0); w0(0);
		for (k=0;k<3;k++) {
			w2(8);	a = r1(); w2(0);
			w2(8); w2(8); w2(8); w2(8); w2(8);
			b = r1(); w2(0);
			if (j44(b,a) != wnt[k]) e++;
		}
		break;

	case 1: w0(0x50); w2(1); w2(0);
		for (k=0;k<3;k++) {
			w2(0xe0); w2(0xe8);
			if (r0() != wnt[k]) e++;
			w2(0xe0); w2(0xe8);
		}

	}
	
	t348_disconnect(pha);

	return e;
}

/* The T348 appears to contain a NCR 5380 core.	 The following
   functions use the 5380 registers. See NCR5380.h for clues.
*/

#define WR(r,v)		t348_write_regr(pha,r,v)
#define RR(r)		(t348_read_regr(pha,r))

static int t348_select (PHA *pha, int initiator, int target)
{
	WR(3,0); WR(1,1);
	WR(0,(1 << initiator));	 WR(2,1);  udelay(100);
	if (RR(1) != 0x41) {
		WR(1,0);
		return -1;
	}

	WR(1,5); WR(0,(1 << initiator)|(1 << target));
	WR(2,0); WR(2,0); WR(2,0);
	return 0;
}

static int t348_test_select (PHA *pha)
{
	return ((RR(4) & 0x42) == 0x42);
}

static void t348_select_finish (PHA *pha)
{
	WR(3,2); WR(1,5); WR(1,1);
}

static void t348_deselect (PHA *pha)
{
	WR(1,0);
}

static int t348_get_bus_status (PHA *pha)
{
	int s;

	s = RR(4);
	return t348_map[s];
}

static void t348_slow_start (PHA *pha, char *val)
{
	int ph, io;

	ph = ((RR(4)>>2)&7);
	io = (ph & 1);

	WR(3,ph);
	WR(1,1-io);
	if (io) *val = RR(0); else WR(0,*val);
	WR(1,0x10+(1-io));
}

static int t348_slow_done (PHA *pha)
{
	return ((RR(4) & 0x20) == 0);
}

static void t348_slow_end (PHA *pha)
{
	int io;

	io = ((RR(4)>>2)&1);

	WR(1,1-io);
}

static void t348_start_block (PHA *pha, int rd)
{
	if (rd) {

		WR(3,1); WR(1,0);
		WR(2,2); WR(7,3);
		WR(3,1); WR(1,0);

		switch (pha->mode) {

		case 0:	w0(0x31); w2(1); w2(0); w0(0x80); w2(8); 
			break;

		case 1: w0(0x21); w2(1); w2(0); w2(0xe8);
			break;
		} 

	} else {

		WR(3,0); WR(1,1);
		WR(2,2); WR(5,0);
		WR(3,0); WR(1,1);

		w0(0x61); w2(1); w2(0);
	}
}

static int t348_transfer_ready (PHA *pha)
{
	if (r1() & 0x80) return 1;

	if (pha->data_dir == 0) return 0;
	return -1;
}

static int t348_transfer_block (PHA *pha, char * buf, int buflen, int rd)
{
	int k, a, b;

	k = 0;
	while (k < buflen) {

	    if (t348_transfer_ready(pha) <= 0) break;

	    if (rd) {
		switch(pha->mode) {

		case 0:	a = r1(); w0(0); b = r1(); w0(0xc0);
			buf[k++] = j44(a,b);
			a = r1(); w0(0x40); b = r1(); w0(0x80);
			buf[k++] = j44(a,b);
			break;

		case 1: buf[k++] = r0(); w2(0xea);
			buf[k++] = r0(); w2(0xe8);
			break;
		}

	    } else {

		w0(buf[k++]); w2(2);
		w0(buf[k++]); w2(0);
	    }	    

	}

	return k;
}

static int t348_transfer_done (PHA *pha)
{
       return 1;
}

static void t348_end_block (PHA *pha, int rd)
{
	w2(0);
	WR(2,0);
}


static void t348_reset_bus (PHA *pha)
{
	WR(1,1); WR(3,0);
	WR(2,0);
	WR(1,0x80); udelay(60);
	WR(1,0);
	WR(2,0);
	WR(1,1); WR(3,0);
	WR(2,0);
}

static char *(mode_strings[2]) = {"Nybble","PS/2"};

static struct ppsc_protocol t348_psp =	{

	{&host0,&host1,&host2,&host3},		/* params	 */
	&host_structs,				/* hosts	 */
	2,					/* num_modes	 */
	2,					/* epp_first	 */
	1,					/* default_delay */
	1,					/* can_message	 */
	0,					/* sg_tablesize	 */
	mode_strings,
	t348_init,
	NULL,
	t348_connect,
	t348_disconnect,
	t348_test_proto,
	t348_select,
	t348_test_select,
	t348_select_finish,
	t348_deselect,
	t348_get_bus_status,
	t348_slow_start,
	t348_slow_done,
	t348_slow_end,
	t348_start_block,
	t348_transfer_block,
	t348_transfer_ready,
	t348_transfer_done,
	t348_end_block,
	t348_reset_bus
};

int t348_detect (struct scsi_host_template *tpnt)
{
	return ppsc_detect( &t348_psp, tpnt, verbose);
}

#ifdef MODULE

struct scsi_host_template	driver_template = PPSC_TEMPLATE(t348);

#include "scsi_module.c"

MODULE_LICENSE("GPL");

#else

void t348_setup (char *str, int *ints)
{
	ppsc_gen_setup(stt,4,str);
}

#endif

/* end of t348.c */


