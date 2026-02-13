/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/*----------------------------------------------*/

#define	JD_SZBUF		512	/* Size of stream input buffer */
#define JD_FORMAT		0	/* 0: RGB888, 1: RGB565 */
#define	JD_USE_SCALE	1	/* Use 1/2, 1/4, 1/8 scaling */
#define JD_TBLCLIP		1	/* Use table for saturation (faster but +1KB) */
#define JD_FASTDECODE	1	/* Fast decoding (0:basic, 1:fast, 2:faster with more memory) */
