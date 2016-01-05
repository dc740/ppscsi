/*
	onscsi.c	(c) 1999 Grant Guenther <grant@torque.net>

	This is the ppSCSI protocol module for the OnSpec 90c26
	in its SCSI adapter mode.
*/

#define	ONSCSI_VERSION	"0.91"

#define PPSC_BASE
#define PPSC_HA_MODULE

#include "ppscsi.h"

#define ONSCSI_REP_COUNT	256

#define	TOGL	pha->private[0]

static char onscsi_map[256];	/* status bits permutation */

static void onscsi_init (PHA *pha)
{

/*			 { REQ, BSY, MSG,  CD,	IO}	*/

	char key[5] = {0x10,0x01,0x20,0x40,0x80};

	ppsc_make_map(onscsi_map,key,0);
	sprintf(pha->ident,"onscsi %s (%s), OnSpec 90c26",
		ONSCSI_VERSION,PPSC_H_VERSION);
}

#define j44(a,b)		((b&0xf0)|((a>>4)&0x0f))

#define CMD(x)	w0(x);w2(5);w2(0xd);w2(5);w2(0xd);w2(5);w2(4);
#define VAL(v)	w0(v);w2(5);w2(7);w2(5);w2(4);

static inline void onscsi_opcode (PHA *pha, int x )
{
	if (pha->mode < 2) {
		CMD(x);
	} else {
		w3(x);
	}
}

#define OP(x)		onscsi_opcode(pha,x) 
#define FULLBYTE	(pha->mode > 0)

static void onscsi_write_regr (PHA *pha, int r, int v)
{
	onscsi_opcode(pha,r);

	if (pha->mode < 2) {	
		VAL(v);
	} else {
		w2(5); w4(v); w2(4); 
	}
}

static inline int onscsi_read_nybble (PHA *pha)
{
	int a, b;

	w2(6); a = r1(); w2(4);
	w2(6); b = r1(); w2(4);

	return j44(a,b);
}

static int onscsi_read_regr (PHA *pha, int r)
{
	int v = -1;

	onscsi_opcode(pha,r);

	switch (pha->mode) {

	case 0:	v = onscsi_read_nybble(pha); 
		break;

	case 1: w2(0x26); v = r0(); w2(4);
		break;

	case 2:
	case 3:
	case 4:	w2(0x24); v = r4(); w2(4);
		break;

	}

	return v;
}

#define	RR(r)		onscsi_read_regr(pha,r)
#define WR(r,v)		onscsi_write_regr(pha,r,v)

static void onscsi_write_block (PHA *pha, char *buf, int n)
{
	int i;

	w2(5+TOGL);

	switch (pha->mode) {

	case 0:
	case 1:	for (i=0;i<n;i++) {
			w0(buf[i]); 
			TOGL = 2 - TOGL;
			w2(5 + TOGL);
			}
		break;

	case 2:	for (i=0;i<n;i++) w4(buf[i]);
		break;

	case 3: for (i=0;i<(n/2);i++) w4w(((u16 *)buf)[i]);
		if (n%2) w4(buf[n-1]);
		break;

	case 4: for (i=0;i<(n/4);i++) w4l(((u32 *)buf)[i]);
		for (i=(n-(n%4));i<n;i++) w4(buf[i]);
		break;

	}

}

static void onscsi_read_block (PHA *pha, char *buf, int n)
{
	int i;

	w2(0x24 + TOGL);

	switch (pha->mode) {

	case 0:	w2(4);
		for (i=0;i<n;i++) buf[i] = onscsi_read_nybble(pha);
		break;

	case 1:	for (i=0;i<n;i++) {
			TOGL = 2 - TOGL;
			w2(0x24 + TOGL); 
			buf[i] = r0();
			}
		break;

	case 2:	for (i=0;i<n;i++) buf[i] = r4();
		break;

	case 3:	for (i=0;i<(n/2);i++) ((u16 *) buf)[i] = r4w();
		if (n%2) buf[n-1] = r4();
		break;

	case 4:	for (i=0;i<(n/4);i++) ((u32 *) buf)[i] = r4l();
		for (i=(n-(n%4));i<n;i++) buf[i] = r4();
		break;

	}

}

#define CPP(x,y)	w0(0xfe);w0(0xaa);w0(0x55);w0(0);w0(0xff);\
			w0(0x87);w0(0x78);w0(x);w2(y|1);w2(y);w0(0xff);

static void onscsi_connect (PHA *pha)
{
	pha->saved_r0 = r0();
	pha->saved_r2 = r2();

	CPP(0x20,4);

	CMD(2); VAL(0);
	CMD(2); VAL(FULLBYTE);

	WR(2,FULLBYTE);
}

static void onscsi_disconnect (PHA *pha)
{
	WR(3,0); WR(7,0x48);
	OP(4); 
	CPP(0x30,pha->saved_r2);

	w0(pha->saved_r0);
	w2(pha->saved_r2);
}

static int onscsi_test_proto (PHA *pha)
{
	int i, k, j;
	char wbuf[16], rbuf[16];
	int e = 0;

	pha->saved_r0 = r0();
	pha->saved_r2 = r2();

	CPP(0x30,pha->saved_r2); 
	CPP(0x0,pha->saved_r2);

	w0(0xfe);w0(0xaa);w0(0x55);w0(0);w0(0xff);
	i = ((r1() & 0xf0) << 4); w0(0x87);
	i |= (r1() & 0xf0); w0(0x78);
	w0(0x20);w2(5);
	i |= ((r1() & 0xf0) >> 4);
	w2(4);w0(0xff);

	if (V_PROBE) printk("%s: signature 0x%x\n",pha->device,i);

	if (i == 0xb5f) {

		CMD(2); VAL(FULLBYTE);
	
		w2(4); w2(0xc); udelay(100); w2(4); udelay(100);

		CMD(2); VAL(0); 
		CMD(2); VAL(FULLBYTE); 

		WR(2,FULLBYTE);

		k = RR(4);
	
		if (V_PROBE)
			printk("%s: OnSpec 90c26 version %x\n",pha->device,k); 
	
	}

	CPP(0x30,pha->saved_r2);

	w0(pha->saved_r0);
	w2(pha->saved_r2);

	if (i != 0xb5f) return 1;

	onscsi_connect(pha);

	for (k=0;k<ONSCSI_REP_COUNT;k++) {

		for (j=0;j<16;j++) wbuf[j] = (k+j)%256;

		if (pha->mode == 0) WR(2,0x30);
		if (pha->mode == 1) WR(2,0x10);

		WR(3,0); WR(7,0x48);

		WR(3,1); WR(7,0x48); OP(5);
		TOGL = 0;
		onscsi_write_block(pha,wbuf,16);
		w2(4);

		if (pha->mode == 0) WR(2,0);
		if (pha->mode == 1) WR(2,0x11);

		WR(3,5); WR(7,0x48); OP(5);
		TOGL = 0;
		onscsi_read_block(pha,rbuf,16);
		w2(4);

		for (j=0;j<16;j++)
			if (rbuf[j] != wbuf[j]) e++;

	}

	onscsi_disconnect(pha);

#ifdef EXPERIMENT 

	/* enable this to see how the buffer status bits work */

	if (pha->mode == 2) {

		onscsi_connect(pha);

		WR(3,0); WR(7,0x48);
		WR(3,1); WR(7,0x48); OP(5);
		w2(5);

		for (k=0;k<16;k++) {
			j = r1(); w4(k);
			printk("%2x:%d ",j,k);			
		}
		printk("\n");

		w2(4); WR(3,5); WR(7,0x48); OP(5);
		w2(0x24);

		for (k=0;k<16;k++) {
			j = r1(); 
			printk("%2x:%d ",j,r4());
		}
		printk("\n");

		w2(4);

		onscsi_disconnect(pha);
	}

	if (pha->mode == 1) {

		onscsi_connect(pha);

		WR(2,0x11);
		WR(3,0); WR(7,0x48);
		WR(3,1); WR(7,0x48); OP(5);
		w2(5);	i = 0;

		for (k=0;k<16;k++) {
			j = r1(); w0(k); i = 2 - i; w2(5+i);
			printk("%2x:%d ",j,k);
		}
		printk("%2x.\n",r1());

		w2(4); 

		WR(2,0x11);
		WR(3,0); WR(7,0x48);
		WR(3,5); WR(7,0x48); OP(5);
		w2(0x24); i = 0;

		printk("%2x  ",r1());
		for (k=0;k<16;k++) {
			i = 2 - i; w2(0x24+i); j = r1();
			printk("%2x:%d ",j,r0());
		}
		printk("\n");

		w2(4);

		onscsi_disconnect(pha);
	}

#endif

	if (V_FULL)
		printk("%s: test port 0x%x mode %d errors %d\n",
		       pha->device,pha->port,pha->mode,e);

	return e;
}

static int onscsi_select (PHA *pha, int initiator, int target)
{
	WR(1,0);
	WR(2,0x80+FULLBYTE);
	if (RR(1) != 0) return -1;
	WR(0,((1 << initiator) | (1 << target)));
	WR(1,2);
	return 0;
}

static int onscsi_test_select (PHA *pha)
{
	return ((RR(1) & 3) == 3);
}

static void onscsi_select_finish (PHA *pha)
{
	WR(1,0);
}

static void onscsi_deselect (PHA *pha)
{
	WR(1,0); 
	/* WR(2,0x20+FULLBYTE); */
	WR(2,FULLBYTE);
	WR(3,0); WR(7,0x48);
}

static int onscsi_get_bus_status (PHA *pha)
{
	WR(2,0x20+FULLBYTE);
	return onscsi_map[RR(1)];
}

static void onscsi_slow_start (PHA *pha, char *val)
{
	pha->priv_flag = (RR(1) & 0x80);
	pha->priv_ptr = val;

	if (pha->priv_flag) WR(2,0x20); else WR(2,0x21);

	OP(0);

	if (pha->priv_flag) {
		w2(6);
	} else { 
		w0(*val); w2(5); w2(7); 
	}
}

static int onscsi_slow_done (PHA *pha)
{
	return (!(r1() & 8));
}

static void onscsi_slow_end (PHA *pha)
{
	if (pha->priv_flag) {
		 *pha->priv_ptr = onscsi_read_nybble(pha);
	} else {
		 w2(5); w2(4);
	}
}

static void onscsi_start_block (PHA *pha, int rd)
{
	pha->priv_flag = rd;

	if (rd) {
		WR(3,5); WR(7,0x48);
		if (pha->mode == 1) WR(2,0x31);
		OP(5);
		w2(5); w0(0xff); w2(4);
	} else {
		WR(3,1); WR(7,0x48);
		if (pha->mode == 1) WR(2,0x31);
		OP(5);
	}
	TOGL = 0;
}

static int onscsi_transfer_done (PHA *pha)
{
	int x;

	if (pha->priv_flag) return 1;

	if (pha->mode == 0) { WR(2,0x20); OP(5); }
	x = r1(); x = r1();
	if (pha->mode == 0) { WR(2,0x30); OP(5); }

	if ((x & 0xf0) == 0x80) return 16;
	return 0;
}

static int onscsi_transfer_ready (PHA *pha)
{
	int x;

	if (pha->priv_flag) {
		x = r1();  x = r1();
		if ((x & 0xf0) == 0xf0) return 16;
		if ((x & 0xf0) == 0xb0) return 8; 
		if ((x & 0xf0) == 0x90) return 1; 
		if ((x & 0xf8) == 0x88) return -1; 
		if ((x & 0xf8) == 0x08) return -1; 
		if ((x & 0xf8) == 0x0) return 1; 

		if ((x & 0xf8) != 0x80) printk("DEBUG: %x\n",x);

		return 0;
	}

	return onscsi_transfer_done(pha);
}


static int onscsi_transfer_block (PHA *pha, char * buf, int buflen, int rd)
{
	int k, b;

	k = 0;
	while ( k < buflen) { 

		if ((b=onscsi_transfer_ready(pha)) <= 0) break;
		if (b > (buflen-k)) b = buflen-k;

		if (rd) onscsi_read_block(pha,buf,b);
		else onscsi_write_block(pha,buf,b);

		k += b; buf += b;
	}

	return k;
}

static void onscsi_end_block (PHA *pha, int rd)
{
	w2(4); WR(3,0); WR(7,0x48);
}

static void onscsi_reset_bus (PHA *pha)
{
	WR(2,2);
	udelay(500);
	WR(2,0);
	WR(2,FULLBYTE);
}

static char *(mode_strings[5]) = {"Nybble","PS/2","EPP","EPP-16","EPP-32"};

static struct ppsc_protocol onscsi_psp =  {

	{&host0,&host1,&host2,&host3},		/* params	 */
	&host_structs,				/* hosts	 */
	5,					/* num_modes	 */
	2,					/* epp_first	 */
	1,					/* default_delay */
	1,					/* can_message	 */
	16,					/* sg_tablesize	 */
	mode_strings,
	onscsi_init,
	NULL,  /* release */
	onscsi_connect,
	onscsi_disconnect,
	onscsi_test_proto,
	onscsi_select,
	onscsi_test_select,
	onscsi_select_finish,
	onscsi_deselect,
	onscsi_get_bus_status,
	onscsi_slow_start,
	onscsi_slow_done,
	onscsi_slow_end,
	onscsi_start_block,
	onscsi_transfer_block,
	onscsi_transfer_ready,
	onscsi_transfer_done,
	onscsi_end_block,
	onscsi_reset_bus
};

int onscsi_detect (struct scsi_host_template *tpnt )
{
	return ppsc_detect( &onscsi_psp, tpnt, verbose);
}

#ifdef MODULE

struct scsi_host_template	driver_template = PPSC_TEMPLATE(onscsi);

#include "scsi_module.c"

MODULE_LICENSE("GPL");

#else

void onscsi_setup (char *str, int *ints)
{
	ppsc_gen_setup(stt,4,str);
}

#endif

/* end of onscsi.c */

