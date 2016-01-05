/*
	epsa2.c	(c) 1996-1999 Grant Guenther <grant@torque.net>

	This is the ppSCSI protocol module for the Shuttle
	Technologies EPSA2 parallel port SCSI adapter.	EPSA2 is
	a predecessor to the EPST.  It uses slightly different
	command encoding and has a less elaborate internal register
	model.

*/

#define	EPSA2_VERSION	"0.91"

#define PPSC_BASE
#define PPSC_HA_MODULE

#include "ppscsi.h"

#define EPSA2_VER_CODE	0xb1

#define j44(a,b)		(((a>>4)&0x0f)+(b&0xf0))
#define j53(a,b)		(((a>>3)&0x1f)+((b<<4)&0xe0))

static char epsa2_map[256];	/* status bits permutation */

static void epsa2_init( PHA *pha)
{

/*			 { REQ, BSY, MSG,  CD,	IO}	*/

	char key[5] = {0x20,0x40,0x04,0x02,0x01};

	ppsc_make_map(epsa2_map,key,0);
	sprintf(pha->ident,"epsa2 %s (%s), Shuttle EPSA2",
		EPSA2_VERSION,PPSC_H_VERSION);
}

static void epsa2_write_regr (PHA *pha, int regr, int value)
{
	switch (pha->mode) {

	case 0:
	case 1:
	case 2: w0(0x70+regr); w2(1); w0(value); w2(4);
		break;

	case 3:
	case 4:
	case 5: w3(0x40+regr); w4(value); w2(4);
		break;

	}
}

static int epsa2_read_regr (PHA *pha, int regr)
{
	int  a, b;

	switch (pha->mode) {

	case 0: w0(0x40+regr); w2(1); w2(3); 
		a = r1(); w2(4); b = r1();
		return j44(a,b);

	case 1: w0(0x60+regr); w2(1); w2(4);
		a = r1(); b = r2(); w0(0xff);
		return j53(a,b);

	case 2: w0(0x50+regr); w2(1); w2(0x25);
		a = r0(); w2(4);
		return a;

	case 3:
	case 4:
	case 5: w3(regr); w2(0x24); a = r4(); w2(4);
		return a;

	}

	return -1;
}

/* for performance reasons, these block transfer functions make 
   some assumptions about the behaviour of the SCSI devices.  In
   particular, DMA transfers are assumed not to stall within the
   last few bytes of a block ...
*/

static int epsa2_read_block (PHA *pha, char *buf, int len)
{
	int t, k, p, a, b;

	k = 0;

	switch (pha->mode) {

	case 0: w0(7); w2(1); w2(3); w0(0xff);
		p = 1;
		while (k < len) {
			w2(6+p); a = r1();
			if (a & 8) b = a; else { w2(4+p); b = r1(); }
			buf[k++] = j44(a,b);
			p = 1 - p;
			if (!(k % 16)) {
				w0(0xfe); t = r1(); w0(0xff);
				if (t & 8) break;
			}
		}	
		w0(0); w2(4);
		break;

	case 1: w0(0x27); w2(1); w2(5); w0(0xff);
		p = 0;
		while (k < len) {
			a = r1(); b = r2(); 
			buf[k++] = j53(a,b);
			w2(4+p);
			p = 1 - p;
			if (!(k % 16)) {
				w0(0xfe); t = r1(); w0(0xff);
				if (t & 8) break;
			}
		}
		w0(0); w2(4);
		break;

	case 2: w0(0x17); w2(1);
		p = 1;
		while (k < len) {
			w2(0x24+p);
			buf[k++] = r0();
			p = 1 - p;
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(6); w2(4);
		break;

	case 3: w3(6); w2(0x24);
		while (k < len) {
			buf[k++] = r4();
			if ((!(k % 16)) && (r1() & 8)) break;
		} 
		w2(4);
		break;

	case 4: w3(6); w2(0x24);
		while (k < len) {
			if ((len - k) > 1) {
				*((u16 *)(&buf[k])) = r4w();
				k += 2;
			} else {
				buf[k++] = r4();
			} 
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(4);
		break;

	case 5: w3(6); w2(0x24);
		while (k < len) {
			if ((len - k) > 3) {
				*((u32 *)(&buf[k])) = r4l();
				k += 4;
			} else {
				buf[k++] = r4();
			}
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(4);
		break;
	}

	return k;
}

static int epsa2_write_block (PHA *pha, char *buf, int len)
{
	int p, k;

	k = 0;

	switch (pha->mode) {

	case 0:
	case 1:
	case 2: w0(0x37); w2(1);
		p = 1;
		while (k < len) {
			w2(4+p);
			w0(buf[k++]);
			p = 1 - p;
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(5); w2(7); w2(4);
		break;

	case 3: w3(0x46); 
		while (k < len) {
			w4(buf[k++]);
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(4);
		break;

	case 4: w3(0x46);
		while (k < len) {
			if ((len - k) > 1) {
				w4w(*((u16 *)(&buf[k])));
				k += 2;
			} else {
				w4(buf[k++]); 
			} 
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(4);
		break;

	case 5: w3(0x46);
		while (k < len) {
			if ((len - k) > 3) {
				w4l(*((u32 *)(&buf[k])));
				k += 4;
			} else {
				w4(buf[k++]); 
			} 
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(4);
		break;
	}

	return k;
}

#define WR(r,v)		epsa2_write_regr(pha,r,v)
#define RR(r)		(epsa2_read_regr(pha,r))

#define CPP(x)	w2(4);w0(0x22);w0(0xaa);w0(0x55);w0(0);w0(0xff);\
		w0(0x87);w0(0x78);w0(x);w2(5);w2(4);w0(0xff);

static void epsa2_connect (PHA *pha)
{
	CPP(0x40); CPP(0xe0);

	w0(0x73); w2(1); w0(0); w2(4);
	w0(0x72); w2(1); w0(0x40); w2(4);

	w0(0); w2(1); w2(4);

	CPP(0x50); CPP(0x48);

	switch (pha->mode) {

	case 0:	WR(7,0x82); 
		break;

	case 1:
	case 2: WR(7,0xa2); 
		break;

	case 3:
	case 4:
	case 5: CPP(0x30); CPP(0x20);
		WR(7,0xa3);
		break;
	}
		
	w2(4);
}

static void epsa2_disconnect (PHA *pha)
{
	switch (pha->mode) {

	case 0:	WR(7,2); WR(2,0);
		break;

	case 1:
	case 2: WR(7,0x22); WR(2,0);
		break;

	case 3:
	case 4:
	case 5: WR(7,0x23); w2(4);
		w0(0x72); w2(1); w0(0); w2(4);
		break;
	}

	CPP(0x30); CPP(0x40); 
}

static int epsa2_test_proto (PHA *pha)
{
	int i, j, e;
	char wb[16], rb[16];

	e = 0;

	epsa2_connect(pha);
	i = RR(7);
	if (V_PROBE) printk("%s: version code reads: 0x%x\n",pha->device,i);
	epsa2_disconnect(pha);

	if (i != EPSA2_VER_CODE) return 1;

	epsa2_connect(pha);

	for (j=0;j<200;j++) {
		for (i=0;i<16;i++) { wb[i] = i+j; rb[i] = i+j+6; }
		WR(5,1);
		epsa2_write_block(pha,wb,16);
		udelay(100);
		WR(5,0x11);
		epsa2_read_block(pha,rb,16);
		for (i=0;i<16;i++) if (wb[i] != rb[i]) e++;
	}

	epsa2_disconnect(pha);

	if (V_FULL)
		printk("%s: test port 0x%x mode %d errors %d\n",
		       pha->device,pha->port,pha->mode,e);

	return e;		
}

/* The EPSA2 contains a core SCSI controller that is very
   similar to the NCR 5380.  Some bits have been shuffled
   around, but the basic structure is the same.
*/

static int epsa2_select (PHA *pha, int initiator, int target)
{
	WR(4,(1<<initiator));
	WR(5,0); WR(1,0); WR(2,0x40); 

	WR(3,0); WR(1,1);
	WR(0,(1<<initiator)); WR(2,0x41); udelay(100);
	if (RR(1) != 0x41) {
		WR(1,0);
		return -1;
	}

	WR(1,5); WR(0,(1<<initiator)|(1<<target));
	WR(2,0x40); WR(2,0x40); WR(2,0x40);

	return 0;
		
}

static int epsa2_test_select (PHA *pha)
{
	return ((RR(4) & 0x50) == 0x50);
}

static void epsa2_select_finish (PHA *pha)
{
	WR(3,2); WR(1,5); WR(1,1);
}

static void epsa2_deselect (PHA *pha)
{
	WR(1,0); WR(2,0x40); WR(3,0);
}

static int epsa2_get_bus_status (PHA *pha)
{
	return epsa2_map[RR(4)];
}

static void epsa2_slow_start (PHA *pha, char *val)
{
	int ph, io;

	ph = RR(4) & 7;
	io = ph & 1;

	WR(3,ph);
	WR(1,1-io);
	if (io) *val = RR(0); else WR(0,*val);
	WR(1,0x11-io);
}

static int epsa2_slow_done (PHA *pha)
{
	return ((RR(4) & 0x20) == 0);
}

static void epsa2_slow_end (PHA *pha)
{
	WR(1,1-(RR(4)&1));
}

static void epsa2_start_block (PHA *pha, int rd)
{
	WR(5,0);

	if (rd)	{

		WR(3,1); WR(1,0);
		WR(5,0x55); WR(2,0x42);

	} else	{

		WR(3,0); WR(1,1);
		WR(5,0x45); WR(2,0x42);

	}
}

static int epsa2_transfer_ready (PHA *pha)
{
	int r;

	r = RR(5);

	if (r & 0x10) return 1;		/* ring buffer half ready */
	if ((!(r & 8)) && (r & 0x20)) return 1;	 /* last fragment */
	if (!(r & 8)) return -1;	/* phase change */
	return 0;
}

static int epsa2_transfer_done (PHA *pha)
{
	if (RR(5) & 0x20) return 0;		/* ring buffer not empty */
	return 1;
}

static int epsa2_transfer_block (PHA *pha, char * buf, int buflen, int rd)
{
	if (epsa2_transfer_ready(pha) <= 0) return 0;

	if (rd) return epsa2_read_block(pha,buf,buflen);
	else	return epsa2_write_block(pha,buf,buflen);
}

static void epsa2_end_block (PHA *pha, int rd)
{
	WR(2,0x40); WR(3,0); WR(1,0);
}

static void epsa2_reset_bus (PHA *pha)
{
	WR(1,1); WR(3,0);
	WR(2,0x40);
	WR(1,0x80); udelay(60);
	WR(1,0);
	WR(2,0x40);
	WR(1,1); WR(3,0);
	WR(2,0x40);
}

static char *(mode_strings[6]) = {"Nybble","5/3","PS/2","EPP","EPP-16","EPP-32"};

static struct ppsc_protocol epsa2_psp =	 {

	{&host0,&host1,&host2,&host3},		/* params	 */
	&host_structs,				/* hosts	 */
	6,					/* num_modes	 */
	3,					/* epp_first	 */
	1,					/* default_delay */
	1,					/* can_message	 */
	16,					/* sg_tablesize	 */
	mode_strings,
	epsa2_init,
	NULL,
	epsa2_connect,
	epsa2_disconnect,
	epsa2_test_proto,
	epsa2_select,
	epsa2_test_select,
	epsa2_select_finish,
	epsa2_deselect,
	epsa2_get_bus_status,
	epsa2_slow_start,
	epsa2_slow_done,
	epsa2_slow_end,
	epsa2_start_block,
	epsa2_transfer_block,
	epsa2_transfer_ready,
	epsa2_transfer_done,
	epsa2_end_block,
	epsa2_reset_bus
};

int epsa2_detect (struct scsi_host_template *tpnt)
{
	return ppsc_detect( &epsa2_psp, tpnt, verbose);
}

#ifdef MODULE

struct scsi_host_template	driver_template = PPSC_TEMPLATE(epsa2);

#include "scsi_module.c"

MODULE_LICENSE("GPL");

#else

void epsa2_setup (char *str, int *ints)
{
	ppsc_gen_setup(stt,4,str);
}

#endif

/* end of epsa2.c */

