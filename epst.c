/*
	epst.c	(c) 1996-1999 Grant Guenther <grant@torque.net>

	This is the ppSCSI protocol module for the Shuttle
	Technologies EPST parallel port SCSI adapter.

*/

#define	EPST_VERSION	"0.92"

#define PPSC_BASE
#define PPSC_HA_MODULE

#include "ppscsi.h"

#define EPST_VER_CODE	0xb2

#define j44(a,b)                (((a>>4)&0x0f)+(b&0xf0))
#define j53(a,b)                (((a>>3)&0x1f)+((b<<4)&0xe0))

static char epst_map[256];	/* status bits permutation */

static void epst_init (PHA *pha)
{

/*			 { REQ, BSY, MSG,  CD,	IO}	*/

	char key[5] = {0x20,0x40,0x04,0x02,0x01};

	ppsc_make_map(epst_map,key,0);
	sprintf(pha->ident,"epst %s (%s), Shuttle EPST",
		EPST_VERSION,PPSC_H_VERSION);
}

static void epst_write_regr (PHA *pha, int regr, int value)
{
	switch (pha->mode) {

	case 0:
	case 1:
	case 2: w0(0x60+regr); w2(1); w0(value); w2(4);
		break;

	case 3:
	case 4:
	case 5: w3(0x40+regr); w4(value);
		break;

	}
}

static int epst_read_regr (PHA *pha, int regr)
{
	int a, b;

	switch (pha->mode) {

	case 0: w0(regr); w2(1); w2(3);
		a = r1(); w2(4); b = r1();
		return j44(a,b);

	case 1: w0(0x40+regr); w2(1); w2(4);
		a = r1(); b = r2(); w0(0xff);
		return j53(a,b);

	case 2: w0(0x20+regr); w2(1); w2(0x25);
		a = r0(); w2(4);
		return a;

	case 3:
	case 4:
	case 5: w3(regr); w2(0x24); a = r4();
		return a;

	}

	return -1;
}

/* for performance reasons, these block transfer functions make 
   some assumptions about the behaviour of the SCSI devices.  In
   particular, DMA transfers are assumed not to stall within the
   last few bytes of a block ...
*/

static int epst_read_block (PHA *pha, char *buf, int len)
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

	case 1: w0(0x47); w2(1); w2(5); w0(0xff);
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

	case 2: w0(0x27); w2(1);
		p = 1;
		while (k < len) {
			w2(0x24+p);
			buf[k++] = r0();
			p = 1 - p;
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(6); w2(4);
		break;

	case 3: w3(0x80); w2(0x24);
		while (k < len) {
			buf[k++] = r4();
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(4);
		break;

	case 4: w3(0x80); w2(0x24);
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

	case 5: w3(0x80); w2(0x24);
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

static int epst_write_block (PHA *pha, char *buf, int len)
{
	int p, k;

	k = 0;

	switch (pha->mode) {

	case 0:
	case 1:
	case 2: w0(0x67); w2(1);
		p = 1;
		while (k < len) {
			w2(4+p);
			w0(buf[k++]);
			p = 1 - p;
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(5); w2(7); w2(4);
		break;

	case 3: w3(0xc0); 
		while (k < len) {
			w4(buf[k++]);
			if ((!(k % 16)) && (r1() & 8)) break;
		}
		w2(4);
		break;

	case 4: w3(0xc0);
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

	case 5: w3(0xc0);
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

#define WR(r,v)         epst_write_regr(pha,r,v)
#define RR(r)           (epst_read_regr(pha,r))

#define CPP(x)	w2(4);w0(0x22);w0(0xaa);w0(0x55);w0(0);w0(0xff);\
		w0(0x87);w0(0x78);w0(x);w2(5);w2(4);w0(0xff);

static void epst_connect (PHA *pha)
{
	w2(4);
	CPP(0x40); CPP(0xe0);
	w0(0); w2(1); w2(3); w2(4);

	if (pha->mode >= 3) {
		w0(0); w2(1); w2(3); w2(4); w2(0xc);
		w0(0x40); w2(6); w2(7); w2(4);
	}

	WR(0x1d,0x20); WR(0x1d,0);  /* clear the ring buffer */
	WR(0xa,0x1e); 		    /* set up PDMA           */
	WR(0xc,4);                  /* enable status bits    */
	WR(8,2);		    /* deglitch timing	     */
}

static void epst_disconnect (PHA *pha)
{
	CPP(0x30); w2(4);
	CPP(0x40); w2(4);
}

#define Wsr(r,v)        WR(0x18+r,v)
#define Rsr(r)          (RR(0x18+r))

static int epst_test_proto (PHA *pha)
{
	int i, j, e;
	char wb[16], rb[16];

	e = 0;

	epst_connect(pha);
	i = RR(0xb);
	if (V_PROBE) printk("%s: version code reads: 0x%x\n",pha->device,i);
	epst_disconnect(pha);

	if (i != EPST_VER_CODE) return 1;

	epst_connect(pha);

	for (j=0;j<200;j++) {
		for (i=0;i<16;i++) { wb[i] = i+j; rb[i] = i+j+6; }
		Wsr(5,1);
		epst_write_block(pha,wb,16);
		Wsr(5,0x11);
		epst_read_block(pha,rb,16);
		for (i=0;i<16;i++) if (wb[i] != rb[i]) e++;
	}

	epst_disconnect(pha);

	if (V_FULL)
		printk("%s: test port 0x%x mode %d errors %d\n",
		       pha->device,pha->port,pha->mode,e);

	return e;	    	
}

/* The EPST contains a core SCSI controller that is very
   similar to the NCR 5380.  Some bits have been shuffled
   around, but the basic structure is the same.
*/

static int epst_select (PHA *pha, int initiator, int target)
{
	Wsr(4,(1<<initiator));
	Wsr(5,0); Wsr(1,0); Wsr(2,0); 

	Wsr(3,0); Wsr(1,1);
	Wsr(0,(1<<initiator)); Wsr(2,1); udelay(100);
	if (Rsr(1) != 0x41) {
		Wsr(1,0);
		return -1;
	}

	Wsr(1,5); Wsr(0,(1<<initiator)|(1<<target));
	Wsr(2,0); Wsr(2,0); Wsr(2,0);

	return 0;
}

static int epst_test_select (PHA *pha)
{
	return ((Rsr(4) & 0x50) == 0x50);
}

static void epst_select_finish (PHA *pha)
{
	Wsr(3,2); Wsr(1,5); Wsr(1,1);
}

static void epst_deselect (PHA *pha)
{
	Wsr(1,0); Wsr(2,0); Wsr(3,0);
}

static int epst_get_bus_status (PHA *pha)
{
	return epst_map[Rsr(4)];
}

static void epst_slow_start (PHA *pha, char *val)
{
	int ph, io;

	ph = Rsr(4) & 7;
	io = ph & 1;

	Wsr(3,ph);
	Wsr(1,1-io);
	if (io) *val = Rsr(0); else Wsr(0,*val);
	Wsr(1,0x11-io);
}

static int epst_slow_done (PHA *pha)
{
	return ((Rsr(4) & 0x20) == 0);
}

static void epst_slow_end (PHA *pha)
{
	Wsr(1,1-(Rsr(4)&1));
}

static void epst_start_block (PHA *pha, int rd)
{
	Wsr(5,0);

	if (rd)	{

		Wsr(3,1); Wsr(1,0);
		Wsr(5,0x15); Wsr(2,2);

	} else  {

		Wsr(3,0); Wsr(1,1);
		Wsr(5,5); Wsr(2,2);

	}
}

static int epst_transfer_ready (PHA *pha)
{
	int r;

	r = Rsr(5);

	if (r & 0x10) return 1;		/* ring buffer half ready */
	if ((!(r & 8)) && (r & 0x20)) return 1;  /* last fragment */
	if (!(r & 8)) return -1;	/* phase change */
	return 0;
}

static int epst_transfer_done (PHA *pha)
{
	if (Rsr(5) & 0x20) return 0;		/* ring buffer not empty */
	return 1;
}

static int epst_transfer_block (PHA *pha, char * buf, int buflen, int rd)
{
	if (epst_transfer_ready(pha) <= 0) return 0;

	if (rd) return epst_read_block(pha,buf,buflen);
	else    return epst_write_block(pha,buf,buflen);
}

static void epst_end_block (PHA *pha, int rd)
{
	Wsr(2,0); Wsr(3,0); Wsr(1,0);
}

static void epst_reset_bus (PHA *pha)
{
	Wsr(1,1); Wsr(3,0);
	Wsr(2,0);
	Wsr(1,0x80); udelay(60);
	Wsr(1,0);
	Wsr(2,0);
	Wsr(1,1); Wsr(3,0);
	Wsr(2,0);
}

static char *(mode_strings[6]) = {"Nybble","5/3","PS/2","EPP","EPP-16","EPP-32"};

static struct ppsc_protocol epst_psp =  {

 	{&host0,&host1,&host2,&host3}, 		/* params        */
	&host_structs,				/* hosts         */
	6,					/* num_modes     */
	3,					/* epp_first     */
	1,					/* default_delay */
	1,					/* can_message   */
	16,					/* sg_tablesize  */
	mode_strings,
	epst_init,
	NULL,
	epst_connect,
	epst_disconnect,
	epst_test_proto,
	epst_select,
	epst_test_select,
	epst_select_finish,
	epst_deselect,
	epst_get_bus_status,
	epst_slow_start,
	epst_slow_done,
	epst_slow_end,
	epst_start_block,
	epst_transfer_block,
	epst_transfer_ready,
	epst_transfer_done,
	epst_end_block,
	epst_reset_bus
};

int epst_detect (struct scsi_host_template *tpnt)
{
	return ppsc_detect( &epst_psp, tpnt, verbose);
}

#ifdef MODULE

struct scsi_host_template	driver_template = PPSC_TEMPLATE(epst);

#include "scsi_module.c"

MODULE_LICENSE("GPL");

#else

void epst_setup (char *str, int *ints)
{
	ppsc_gen_setup(stt,4,str);
}

#endif

/* end of epst.c */

