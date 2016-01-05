/*
	sparcsi.c	(c) 1997-1999 Grant Guenther <grant@torque.net>

	This is the low-level protocol module for the WBS-11A parallel
	port SCSI adapter.  This adapter has been marketed by LinkSys
	as the "ParaSCSI+" and by Shining Technologies as the "SparCSI".
	The device is constructed from the KBIC-951A ISA replicator
	chip from KingByte and the NCR 5380.

*/

#define SPARCSI_VERSION	"0.91"

#define PPSC_BASE
#define PPSC_HA_MODULE

#include "ppscsi.h"

#define r12w()			(delay_p,inw(pha->port+1)&0xffff) 

#define j44(a,b)		((((a>>4)&0x0f)|(b&0xf0))^0x88)
#define j53(w)			(((w>>3)&0x1f)|((w>>4)&0xe0))

static char sparcsi_map[256];	/* status bits permutation */

static void sparcsi_init (PHA *pha)
{

/*			 { REQ, BSY, MSG,  CD,	IO}	*/

	char key[5] = {0x20,0x40,0x10,0x08,0x04};

	ppsc_make_map(sparcsi_map,key,0);
	sprintf(pha->ident,"sparcsi %s (%s), WBS-11A",
		SPARCSI_VERSION,PPSC_H_VERSION);
}

static void sparcsi_write_regr (PHA *pha, int regr, int value)
{
	switch (pha->mode) {

	case 0: 
	case 1:
	case 2: w0(regr|0x10); w2(4); w2(6); w2(4); 
		w0(value); w2(5); w2(4);
		break;

	case 3: w0(0x20); w2(4); w2(6); w2(4); w3(regr);
		w4(value); w2(4); w2(0); w2(4);
		break;

	}
}

static int sparcsi_read_regr (PHA *pha, int regr)
{
	int a, b;

	switch (pha->mode) {

	case 0: w0(regr|0x18); w2(4); w2(6); w2(4); w2(5); 
		a = r1(); w0(0x58); b = r1(); w2(4);
		return j44(a,b);

	case 1: w0(regr|0x58); w2(4); w2(6); w2(4); w2(5);
		a = r12w(); w2(4);
		return j53(a);

	case 2: w0(regr|0x98); w2(4); w2(6); w2(4); w2(0xa5); 
		a = r0(); w2(4);
		return a;

	case 3: w0(0x20); w2(4); w2(6); w2(4); w3(regr);
		w2(0xe4); a = r4(); w2(4); w2(0); w2(4);
		return a;

	}
	return -1;
}

static void sparcsi_read_block (PHA *pha, char *buf, int len)
{
	int k, a, b;

	switch (pha->mode) {

	case 0: w0(8); w2(4); w2(6); w2(4);
		for (k=0;k<len/2;k++) {
			w2(5); a = r1(); w0(0x48); b = r1(); w2(4);
			buf[2*k] = j44(a,b);
			w2(5); b = r1(); w0(8); a = r1(); w2(4);
			buf[2*k+1] = j44(a,b);
		} 
		break;

	case 1: w0(0x48); w2(4); w2(6); w2(4); 
		for (k=0;k<len;k++) {
			w2(5); buf[k] = j53(r12w()); w2(4); 
		}
		break;

	case 2: w0(0x88); w2(4); w2(6); w2(4);
		for (k=0;k<len;k++) {
			w2(0xa5); buf[k] = r0(); w2(0xa4);
		}
		w2(4);
		break;

	case 3: w0(0x20); w2(4); w2(6); w2(4); w3(6); w2(0xe4);
		for (k=0;k<len;k++) buf[k] = r4();
		w2(4); w2(0); w2(4);
		break;
	
	}
}

static void  sparcsi_write_block (PHA *pha, char *buf, int len)
{
	int k;

	switch (pha->mode) {

	case 0:
	case 1:
	case 2: w0(0); w2(4); w2(6); w2(4); 
		for(k=0;k<len;k++) {
			w0(buf[k]); w2(5); w2(4); 
		}
		break;

	case 3: w0(0x20); w2(4); w2(6); w2(4); w3(6);
		for(k=0;k<len;k++) w4(buf[k]);
		w2(4); w2(0); w2(4);
		break;
	}
}

static void sparcsi_connect (PHA *pha)
{
	pha->saved_r0 = r0();
	pha->saved_r2 = r2();
	w2(4);
}

static void sparcsi_disconnect (PHA *pha)
{
	w0(pha->saved_r0);
	w2(pha->saved_r2);
}

#define WR(r,v)		sparcsi_write_regr(pha,r,v)
#define RR(r)		(sparcsi_read_regr(pha,r))

static int sparcsi_test_proto (PHA *pha)
{
	int k, e;

	e = 0;

	sparcsi_connect(pha);

	if (!pha->private[0]) {	  /* reset the SCSI bus on first sight */

		if (V_FULL) printk("%s: SCSI reset ...\n",pha->device);

		WR(1,0x80); udelay(60);
		WR(1,0); 
		ssleep(5*HZ);
		pha->private[0] = 1;
	}

	WR(1,0);
	WR(1,1);

	if (V_PROBE) 
		printk("%s: 5380 regrs [4]=%x [5]=%x\n",pha->device,RR(4),RR(5));

	for (k=0;k<256;k++) {
		WR(0,k); 
		if (RR(0) != k) e++;
		WR(0,255-k);
		if (RR(0) != (255-k)) e++;
	}

	WR(1,0);

	sparcsi_disconnect(pha);

	return e;
}

static int sparcsi_select (PHA *pha, int initiator, int target)
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

static int sparcsi_test_select (PHA *pha)
{
	return ((RR(4) & 0x42) == 0x42);
}

static void sparcsi_select_finish (PHA *pha)
{
	WR(3,2); WR(1,5); WR(1,1);
}

static void sparcsi_deselect (PHA *pha)
{
	WR(1,0);
}

static int sparcsi_get_bus_status (PHA *pha)
{
	int s;

	s = RR(4);
	return sparcsi_map[s];
}

static void sparcsi_slow_start (PHA *pha, char *val)
{
	int ph, io;

	ph = ((RR(4)>>2)&7);
	io = (ph & 1);

	WR(3,ph);
	WR(1,1-io);
	if (io) *val = RR(0); else WR(0,*val);
	WR(1,0x10+(1-io));
}

static int sparcsi_slow_done (PHA *pha)
{
	return ((RR(4) & 0x20) == 0);
}

static void sparcsi_slow_end (PHA *pha)
{
	int io;

	io = ((RR(4)>>2)&1);

	WR(1,1-io);
}

static void sparcsi_start_block (PHA *pha, int rd)
{
	if (rd) {

		WR(3,1); WR(1,0);
		WR(2,2); WR(7,3);
		WR(3,1); WR(1,0);

	} else {

		WR(3,0); WR(1,1);
		WR(2,2); WR(5,0);
		WR(3,0); WR(1,1);

	}
	pha->priv_flag = rd;
}

static int sparcsi_transfer_ready (PHA *pha)
{
	int chunk;

	chunk = 512;
	if ((pha->data_count == 0) && (!pha->priv_flag)) chunk++;

	if (r1() & 0x40) return chunk;
	if (!(RR(5) & 8)) return -1;
	return 0;
}

static int sparcsi_transfer_block (PHA *pha, char * buf, int buflen, int rd)
{
	int k, n;

	k = 0;
	while (k < buflen) {

		n = sparcsi_transfer_ready(pha);

		if (n <= 0) break;

		if (n > (buflen - k)) n = buflen - k;

		if (rd) sparcsi_read_block(pha,buf,n);
		else  sparcsi_write_block(pha,buf,n);

		k += n; buf += n;
	}

	return k;
}

static int sparcsi_transfer_done (PHA *pha)
{
	return 1;
}

static void sparcsi_end_block (PHA *pha, int rd)
{
	char buf[2] = {0,0};

	if (!rd) sparcsi_write_block(pha,buf,1); 

	WR(2,0);
}

static void sparcsi_reset_bus (PHA *pha)
{
	WR(1,1); WR(3,0);
	WR(2,0);
	WR(1,0x80); udelay(60);
	WR(1,0);
	WR(2,0);
	WR(1,1); WR(3,0);
	WR(2,0);
}

static char *(mode_strings[4]) = {"Nybble","KBIC 5/3","PS/2","EPP"};

static struct ppsc_protocol sparcsi_psp =  {

	{&host0,&host1,&host2,&host3},		/* params	 */
	&host_structs,				/* hosts	 */
	4,					/* num_modes	 */
	3,					/* epp_first	 */
	1,					/* default_delay */
	1,					/* can_message	 */
	16,					/* sg_tablesize	 */
	mode_strings,
	sparcsi_init,
	NULL,
	sparcsi_connect,
	sparcsi_disconnect,
	sparcsi_test_proto,
	sparcsi_select,
	sparcsi_test_select,
	sparcsi_select_finish,
	sparcsi_deselect,
	sparcsi_get_bus_status,
	sparcsi_slow_start,
	sparcsi_slow_done,
	sparcsi_slow_end,
	sparcsi_start_block,
	sparcsi_transfer_block,
	sparcsi_transfer_ready,
	sparcsi_transfer_done,
	sparcsi_end_block,
	sparcsi_reset_bus
};

int sparcsi_detect (struct scsi_host_template *tpnt)
{
	return ppsc_detect( &sparcsi_psp, tpnt, verbose);
}

#ifdef MODULE

struct scsi_host_template	driver_template = PPSC_TEMPLATE(sparcsi);

#include "scsi_module.c"

MODULE_LICENSE("GPL");

#else

void sparcsi_setup (char *str, int *ints)
{
	ppsc_gen_setup(stt,4,str);
}

#endif

/* end of sparcsi.c */


