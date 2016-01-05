/*
	t358.c	(c) 1997-1999 Grant Guenther <grant@torque.net>

	This is the low-level protocol module for the Adaptec APA-358
	(aka Trantor T358) parallel port SCSI adapter.  It forms part
	of the 'ppSCSI' suite of drivers.

*/

#define T358_VERSION	"0.91"

#define PPSC_BASE
#define PPSC_HA_MODULE

#include "ppscsi.h"

#define j44(a,b)                (((a<<1)&0xf0)+((b>>3)&0x0f))

static char t358_map[256];	/* status bits permutation */

static void t358_init (PHA *pha)
{

/*			 { REQ, BSY, MSG,  CD,	IO}	*/

	char key[5] = {0x20,0x40,0x10,0x08,0x04};

	ppsc_make_map(t358_map,key,0);
	sprintf(pha->ident,"t358 %s (%s), Adaptec APA-358",
			T358_VERSION,PPSC_H_VERSION);
}

static void t358_write_regr (PHA *pha, int regr, int value)
{
	int x;

	switch (pha->mode) {

	case 0:
	case 1: w0(regr); x = r2(); w2(1); w2(9); w2(0); w2(0);
		w0(value); w2(1); w2(3); w2(0); w2(x);
		break;

	case 2: w2(0xc0); w3(regr); w4(value);
		break;

	}
}

static int t358_read_regr (PHA *pha, int regr)
{
	int h, l;

	switch (pha->mode) {

	case 0: w0(regr); w2(1); w2(9); w2(0); w2(0); 
		w0(0x80); w2(2); h = r1();
		w0(0); l = r1(); w2(0);
		return j44(h,l);

	case 1: w0(regr); h = r2(); w2(1); w2(9); w2(0); w2(0);
		w2(0xe2); l = r0(); w2(h);
		return l;

	case 2: h = r2(); w2(0xe0); w3(regr); w2(0xe0);
		l = r4(); w2(h);
		return l;
	}

	return 0;
}

static void t358_read_block (PHA *pha, char *buf, int len)
{
	int k, h, l;

	switch (pha->mode) {

	case 0: w0(0x10); w2(1); w2(9); w2(0); w2(0);
		for (k=0;k<len;k++) {
			w0(0x80); w2(2); h = r1();
			w0(0); l = r1(); w2(0);
			buf[k] = j44(h,l);
                }
		break;

        case 1: w0(0x10); w2(1); w2(9); w2(0); w2(0);
		for (k=0;k<len;k++) {
			w2(0xe2);
			buf[k] = r0();
			w2(0xe0);
		}
		break;

	case 2: w2(0xc0); w3(0x10); w2(0xe0);
		for (k=0;k<len;k++) buf[k] = r4();
		w2(0xc0);
		break;
	}
}

static void t358_write_block (PHA *pha, char *buf, int len)
{
	int k, x;

	switch (pha->mode) {

	case 0:
	case 1: w0(0x10); x = r2();
		w2(1); w2(9); w2(0); w2(0);
		for (k=0;k<len;k++) {
			w0(buf[k]);
			w2(1); w2(3); w2(0);
		}
		w2(x);
		break;

	case 2: w2(0xc0); w3(0x10); w2(0xc0);
		for (k=0;k<len;k++) w4(buf[k]);
		break;
	}
}

static void t358_connect (PHA *pha)
{
	int b;

	pha->saved_r0 = r0();
	w0(0);
	pha->saved_r2 = r2();
	b = pha->saved_r2 % 4;
	w0(0xf7); w2(b+4); w2(b); w2(b+8); w2(b); w2(0);

	if (pha->mode) { w0(0x80); w2(1); w2(9); w2(1); w2(0); }
	else { w0(0xa0); w2(1); w2(9); w2(0); }
}

static void t358_disconnect (PHA *pha)
{
	w0(pha->saved_r0);
	w2(pha->saved_r2);
}

static int t358_test_proto (PHA *pha)
{
	int h, l, a, b;
	int j = 0, k = 0, e = 0;

	t358_connect(pha);

	switch (pha->mode) {

	case 0:	w0(0x80); w2(8); h = r1(); w0(0); l = r1();
        	w2(0); w2(8); a = r1(); w0(0); b = r1(); w2(0);
        	k = j44(h,l); j = j44(a,b);
		break;

	case 1: w2(0xe0); w0(0); w2(0xe8); k = r0();
		w2(0xe0); w2(0xe8); j = r0(); w2(0xe0);
		break;

	case 2:	w0(0xa0); w2(1); w2(9); w2(0);
		w0(0x80); w2(8); h = r1(); w0(0); l = r1();
                w2(0); w2(8); a = r1(); w0(0); b = r1(); w2(0);
                k = j44(h,l); j = j44(a,b);
		w0(0x80); w2(1); w2(9); w2(1); w2(0);

	}

	if (V_PROBE) printk("%s: Signature: %x %x\n",pha->device,k,j);

        if ((k != 0xe8) || (j != 0xff)) e++;

	t358_disconnect(pha);

	if (!e) {

	    t358_connect(pha);

	    for (j=0;j<256;j++) {
		t358_write_regr(pha,0,j);
		k = t358_read_regr(pha,0);
		if (k != j) e++;
		} 

	    t358_disconnect(pha);

	}

	return e;
}

/* The T358 appears to contain a NCR 53c400 core.  Check NCR5380.h
   for hints about the regrs ...  */

#define WR(r,v)         t358_write_regr(pha,r+8,v)
#define RR(r)           (t358_read_regr(pha,r+8))

static int t358_select (PHA *pha, int initiator, int target)
{
	WR(3,0); WR(1,1);
	WR(0,(1 << initiator));  WR(2,1);  udelay(100);
	if (RR(1) != 0x41) {
		WR(1,0);
		return -1;
	}

	WR(1,5); WR(0,(1 << initiator)|(1 << target));
	WR(2,0); WR(2,0); WR(2,0);
	return 0;
}

static int t358_test_select (PHA *pha)
{
	return ((RR(4) & 0x42) == 0x42);
}

static void t358_select_finish (PHA *pha)
{
	WR(3,2); WR(1,5); WR(1,1);
}

static void t358_deselect (PHA *pha)
{
	WR(1,0);
}

static int t358_get_bus_status (PHA *pha)
{
	int s;

	s = RR(4);
	return t358_map[s];
}

static void t358_slow_start (PHA *pha, char *val)
{
	int ph, io;

	ph = ((RR(4)>>2)&7);
	io = (ph & 1);

	WR(3,ph);
	WR(1,1-io);
	if (io) *val = RR(0); else WR(0,*val);
	WR(1,0x10+(1-io));
}

static int t358_slow_done (PHA *pha)
{
	return ((RR(4) & 0x20) == 0);
}

static void t358_slow_end (PHA *pha)
{
	int io;

	io = ((RR(4)>>2)&1);

	WR(1,1-io);
}

static void t358_start_block (PHA *pha, int rd)
{
	if (rd) {
		WR(3,1); WR(1,0);
        	WR(2,2); 
		WR(0x10,0x40); WR(2,0); WR(2,0xa);
		WR(3,1); WR(1,0); WR(7,3);
	} else {
		WR(3,0); WR(1,1);
		WR(2,2); 
		WR(0x10,0); WR(2,0); WR(2,0xa);
		WR(3,0); WR(1,1); WR(5,0);
	}
	WR(0x11,pha->tlen/128);
}

static int t358_transfer_ready (PHA *pha)
{
	int r;

	r = RR(0x10);	

	if (!(r & 4)) return 128;	/* 4 is host buffer not ready */
	
	if (r & 1) return -1;		/* last block transferred */

	return 0;
}

static int t358_transfer_block (PHA *pha, char * buf, int buflen, int rd)
{
	int k, n;

	k = 0;
	while (k < buflen) {

	    n = t358_transfer_ready(pha);

	    if (n <= 0) break;

	    if (n > (buflen - k)) n = buflen - k;

	    if (rd) t358_read_block(pha,buf,n);
	    else  t358_write_block(pha,buf,n);

	    k += n; buf += n;

	}

	return k;
}

static int t358_transfer_done (PHA *pha)
{
	if (RR(0x10) & 1) return 1;	/* last block transferred */
	return 0;
}

static void t358_end_block (PHA *pha, int rd)
{
	WR(2,0);
}


static void t358_reset_bus (PHA *pha)
{
	WR(1,1); WR(3,0);
        WR(2,0);
        WR(1,0x80); udelay(60);
        WR(1,0);
        WR(2,0);
        WR(1,1); WR(3,0);
        WR(2,0);
}

static char *(mode_strings[3]) = {"Nybble","PS/2","EPP"};

static struct ppsc_protocol t358_psp =  {

 	{&host0,&host1,&host2,&host3}, 		/* params        */
	&host_structs,				/* hosts         */
	3,					/* num_modes     */
	2,					/* epp_first     */
	1,					/* default_delay */
	1,					/* can_message   */
	16,					/* sg_tablesize  */
	mode_strings,
	t358_init,
	NULL,
	t358_connect,
	t358_disconnect,
	t358_test_proto,
	t358_select,
	t358_test_select,
	t358_select_finish,
	t358_deselect,
	t358_get_bus_status,
	t358_slow_start,
	t358_slow_done,
	t358_slow_end,
	t358_start_block,
	t358_transfer_block,
	t358_transfer_ready,
	t358_transfer_done,
	t358_end_block,
	t358_reset_bus
};

int t358_detect (struct scsi_host_template *tpnt )
{
	return ppsc_detect( &t358_psp, tpnt, verbose);
}

#ifdef MODULE

struct scsi_host_template	driver_template = PPSC_TEMPLATE(t358);

#include "scsi_module.c"

MODULE_LICENSE("GPL");

#else

void t358_setup (char *str, int *ints)
{
	ppsc_gen_setup(stt,4,str);
}

#endif

/* end of t358.c */


