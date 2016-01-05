/*
	vpi0.c	(c) 1995-1999 Grant Guenther <grant@torque.net>
		(c) 1997-1999 David Campbell <campbell@torque.net>

	This is the ppSCSI protocol module for the Iomega VPI0 adapter
	found in the original ZIP-100 drives and the Jaz Traveller.

*/

#define	VPI0_VERSION	"0.91"

#define PPSC_BASE
#define PPSC_HA_MODULE

#include "ppscsi.h"

static char vpi0_map[256];	/* status bits permutation */

static void vpi0_init (PHA *pha)
{

/*  *** No MSG line on the VPI0 ! *** */

/*                       { REQ, BSY, MSG,  CD,  IO}     */

	char key[5] = {0x80,0x40,0x00,0x20,0x10};

	ppsc_make_map(vpi0_map,key,0);
	sprintf(pha->ident,"vpi0 %s (%s) ",VPI0_VERSION,PPSC_H_VERSION);
}

#define	j44(a,b)	((a&0xf0)|((b>>4)&0x0f))

#define CST(v)	w2(0xc);w0(v);w2(4);w2(6);w2(4);w2(0xc);
#define DST(v)	w2(0xc);w0(v);w2(0xc);w2(0xe);w2(0xc);w2(4);w2(0xc);

static void vpi0_connect (PHA *pha)
{
	pha->saved_r0 = r0();
	pha->saved_r2 = r2();

	CST(0); CST(0x3c); CST(0x20);
	if (pha->mode >= 2) { CST(0xcf) } else { CST(0x8f) }
}

static void vpi0_disconnect (PHA *pha)
{
	DST(0); DST(0x3c); DST(0x20); DST(0xf);

	w2(pha->saved_r2);
	w0(pha->saved_r0);
}


/* There are no data-transfer tests available, just this simple 
   check that we are talking to a VPI0.  */
static int vpi0_test_proto (PHA *pha)
{
	int	e = 2;

	vpi0_connect(pha);
	w2(0xe);
	if ((r1() & 8) == 8) e--;
	w2(0xc);
	if ((r1() & 8) == 0) e--;
	vpi0_disconnect(pha);
	return e;
}

static int vpi0_select (PHA *pha, int initiator, int target)
{
	w2(0xc);
	if (r1() & 0x40) return -1;	/* bus busy */

	w0(1<<target);
	w2(0xe); w2(0xc);
	w0(0x80); w2(8);	/* assert SEL */

	return 0;

}

static int vpi0_test_select (PHA *pha)
{
	return (r1() & 0x40);	/* BSY asserted ? */
}

static void vpi0_select_finish (PHA *pha)
{
	w2(0xc);
}

static void vpi0_deselect (PHA *pha)
{
	w2(0xc);
}

static int vpi0_get_bus_status (PHA *pha)
{
	w2(0xc);
	return vpi0_map[r1()];
}

/* These functions are inlined so the C optimiser can move the switches
   outside of loops where possible, am I dreaming ?  */

static inline int vpi0_read (PHA *pha, int first)
{
	int l, h;

	switch (pha->mode) {

	case 0:	if (first) w2(4);
		h = r1(); w2(6);
		l = r1(); w2(4);
		return j44(h,l);

	case 1: if (first) w2(0x25);
		l = r0();
		w2(0x27); w2(0x25);
		return l;

	case 2: if (first) w2(0x24);
		return r4();

	default: return -1;

	}
}

static inline void vpi0_write (PHA *pha, int v, int first )
{
	switch (pha->mode) {

	case 0:
	case 1:	if (first) w2(0xc);
		w0(v); w2(0xe); w2(0xc);
		break;

	case 2:	if (first) w2(0x4);
		w4(v);
		break;

	}
}

static void vpi0_slow_start (PHA *pha, char *val)
{
	int r;

	w2(0xc);

	r = (r1() & 0x10);

	if (r) *val = vpi0_read(pha,1);
	  else vpi0_write(pha,*val,1);

}

static int vpi0_slow_done (PHA *pha)
{
	return 1;  /* vpi0 does its own REQ/ACK handshaking */
}

static void vpi0_slow_end (PHA *pha)
{
	w2(0xc);
}

static void vpi0_start_block (PHA *pha, int rd)
{
	pha->priv_flag = rd;
}

static int vpi0_transfer_ready (PHA *pha)
{
	int b;

	b = vpi0_get_bus_status(pha);
	if ((b & PPSC_PH_STAT) == PPSC_PH_STAT) return -1;
	if (b == (PPSC_REQ|PPSC_BSY| pha->priv_flag)) return 128;
	return 0;
}

static int vpi0_transfer_block (PHA *pha, char * buf, int buflen, int rd)
{
	int k, n, i;

	k = 0;
	while (k < buflen) {
	    n = vpi0_transfer_ready(pha);
	    if (n <= 0 ) break;
	    if (n > (buflen-k)) n = buflen-k;
	    for (i=0;i<n;i++) 
		if (rd) buf[k++] = vpi0_read(pha,!i);
	          else vpi0_write(pha,buf[k++],!i);
	    w2(0xc);
	}
	return k;
}

static int vpi0_transfer_done (PHA *pha)
{
       return 1;
}

static void vpi0_end_block (PHA *pha, int rd)
{
	w2(0xc);
}

static void vpi0_reset_bus (PHA *pha)
{
	w2(0xc);
	w0(0x40); w2(8); udelay(60);
	w2(0xc); 
}

/* Make these correspond to the actual modes supported by the adapter */

static char *(mode_strings[3]) = {"Nybble","PS/2","EPP"};

static struct ppsc_protocol vpi0_psp =  {

 	{&host0,&host1,&host2,&host3}, 		/* params        */
	&host_structs,				/* hosts         */
	3,					/* num_modes     */
	2,					/* epp_first     */
	1,					/* default_delay */
	0,					/* can_message   */
	16,					/* sg_tablesize  */
	mode_strings,
	vpi0_init,
	NULL,
	vpi0_connect,
	vpi0_disconnect,
	vpi0_test_proto,
	vpi0_select,
	vpi0_test_select,
	vpi0_select_finish,
	vpi0_deselect,
	vpi0_get_bus_status,
	vpi0_slow_start,
	vpi0_slow_done,
	vpi0_slow_end,
	vpi0_start_block,
	vpi0_transfer_block,
	vpi0_transfer_ready,
	vpi0_transfer_done,
	vpi0_end_block,
	vpi0_reset_bus
};

int vpi0_detect (struct scsi_host_template *tpnt)
{
	return ppsc_detect( &vpi0_psp, tpnt, verbose);
}

#ifdef MODULE

struct scsi_host_template driver_template = PPSC_TEMPLATE(vpi0);

#include "scsi_module.c"

MODULE_LICENSE("GPL");

#else

void vpi0_setup (char *str, int *ints)
{
	ppsc_gen_setup(stt,4,str);
}

#endif

/* end of vpi0.c */

