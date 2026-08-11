/*
 * SETMCH.PRG - force the _MCH machine-type cookie (Bespoke / APJ-OS).
 *
 * STE-specific software gates on the _MCH cookie: STE = 0x00010000,
 * plain ST = 0x00000000. On a plain-ST PiStorm the cookie reads "ST"
 * and such software refuses to run - even though the emulator provides
 * an STE blitter and STE DMA sound in hardware-register-compatible
 * form. Overwriting _MCH makes cookie-gated STE software run and use
 * those emulated features.
 *
 * This is an AUTO-folder program on purpose: it must work the same in
 * a plain-GEM/TOS boot (games) and a FreeMiNT boot (MOD players, GEM
 * apps), independent of whatever desktop is - or is not - loaded. It
 * simply patches the cookie jar that every environment shares, so
 * anything launched afterwards sees the machine you chose.
 *
 * PLACEMENT: put SETMCH.PRG in the AUTO folder. Under FreeMiNT, name
 * it so it runs AFTER MINT.PRG (alphabetical - 'S' already sorts after
 * 'M'), so MiNT finishes detecting the REAL machine before the cookie
 * is changed; only later-launched programs see the faked value.
 *
 * TWO HONEST LIMITS this does NOT cure (they are hardware/timing, not
 * the cookie): STE *video* - hardware fine scroll, the 4096-colour
 * palette, split-screen - is the real shifter, absent on a plain ST;
 * and cycle-exact software (beam-racing demos, some games) still
 * breaks under the JIT. This unlocks well-behaved cookie-gated STE
 * software - not beam-racing demos.
 *
 * CONFIG: the machine defaults to STE. To pick another, put a one-line
 * file SETMCH.INF in the root of the boot drive containing one of:
 *     ST  STE  MEGASTE  TT  FALCON      (case-insensitive), or
 *     0x00030000                        (a raw hex _MCH value).
 *
 * Requires a cookie jar (EmuTOS, all modern TOS, and FreeMiNT provide
 * one; only TOS 1.00-1.04 lack it - if none is present the tool says
 * so and changes nothing).
 *
 * Clean-room from the public cookie-jar layout (_p_cookies at 0x5A0,
 * array of {long tag; long value} ending with a {0; capacity} slot).
 * Build with the m68k-atari-mint cross toolchain: `make`.
 */

#include <mint/osbind.h>

#define C__MCH		0x5F4D4348L			/* '_MCH' */

#define MCH_ST		0x00000000L
#define MCH_STE		0x00010000L
#define MCH_MEGASTE	0x00010010L
#define MCH_TT		0x00020000L
#define MCH_FALCON	0x00030000L

static long g_val = MCH_STE;			/* desired value (default STE) */


/*
 * The patch, run in supervisor mode via Supexec() so it may touch low
 * memory / the kernel cookie jar under FreeMiNT memory protection.
 * Returns: 1 overwritten, 2 inserted, 0 jar full, -1 no jar.
 */

static long do_patch(void)
{
	long *base = *(long **) 0x5A0L;		/* _p_cookies */
	long *jar = base;

	if (base == 0L)
		return -1L;

	while (jar[0] != 0L)
	{
		if (jar[0] == C__MCH)
		{
			jar[1] = g_val;
			return 1L;
		}
		jar += 2;
	}

	/* jar points at the terminator: tag 0, value = total slot capacity.
	 * insert if two slots remain (new cookie + moved terminator) */
	{
		long used = (jar - base) / 2;
		long cap = jar[1];

		if (used + 2 <= cap)
		{
			jar[2] = 0L;				/* new terminator first */
			jar[3] = cap;
			jar[0] = C__MCH;
			jar[1] = g_val;
			return 2L;
		}
	}

	return 0L;
}


/*
 * Optional override from SETMCH.INF on the boot drive root. Absent or
 * unreadable -> keep the STE default.
 */

static void read_cfg(void)
{
	long fh;
	long n;
	char buf[32];
	int i;

	fh = Fopen("\\SETMCH.INF", 0);		/* read-only */

	if (fh < 0)
		return;

	n = Fread((int) fh, (long) sizeof(buf) - 1, buf);
	Fclose((int) fh);

	if (n <= 0)
		return;

	buf[n] = 0;

	for (i = 0; i < (int) sizeof(buf) && buf[i]; i++)
	{
		char c = buf[i];

		if (c >= 'a' && c <= 'z')
			buf[i] = (char) (c - 32);	/* upper-case in place */
	}

	if (buf[0] == '0' && buf[1] == 'X')
	{
		long v = 0L;

		for (i = 2; buf[i]; i++)
		{
			char c = buf[i];

			if (c >= '0' && c <= '9')
				v = v * 16L + (c - '0');
			else if (c >= 'A' && c <= 'F')
				v = v * 16L + (c - 'A' + 10);
			else
				break;
		}
		g_val = v;
	}
	else if (buf[0] == 'M' && buf[1] == 'E')	/* MEGASTE (before STE) */
		g_val = MCH_MEGASTE;
	else if (buf[0] == 'S' && buf[1] == 'T' && buf[2] == 'E')
		g_val = MCH_STE;
	else if (buf[0] == 'S' && buf[1] == 'T')
		g_val = MCH_ST;
	else if (buf[0] == 'T' && buf[1] == 'T')
		g_val = MCH_TT;
	else if (buf[0] == 'F')
		g_val = MCH_FALCON;
}


int main(void)
{
	long r;

	read_cfg();

	r = Supexec(do_patch);

	Cconws("SETMCH: _MCH cookie ");

	if (r == 1L)
		Cconws("set\r\n");
	else if (r == 2L)
		Cconws("added\r\n");
	else if (r == 0L)
		Cconws("- jar full, unchanged\r\n");
	else
		Cconws("- no cookie jar, unchanged\r\n");

	return 0;
}
