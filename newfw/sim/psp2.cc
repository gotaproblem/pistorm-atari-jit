#include <cxxrtl/cxxrtl.h>

#if defined(CXXRTL_INCLUDE_CAPI_IMPL) || \
    defined(CXXRTL_INCLUDE_VCD_CAPI_IMPL)
#include <cxxrtl/capi/cxxrtl_capi.cc>
#endif

#if defined(CXXRTL_INCLUDE_VCD_CAPI_IMPL)
#include <cxxrtl/capi/cxxrtl_capi_vcd.cc>
#endif

using namespace cxxrtl_yosys;

namespace cxxrtl_design {

// \top: 1
// \src: ../rtl/pistorm_psp2.v:17.1-526.10
struct p_pistorm__psp2__core : public module {
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:272.15-272.22
	wire<3> p_por__cnt;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:262.25-262.35
	wire<1> p_bgack__prev;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:262.9-262.16
	wire<1> p_br__prev;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:261.30-261.39
	wire<8> p_cnt__bgack;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:261.15-261.21
	wire<8> p_cnt__br;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:260.9-260.18
	wire<1> p_dbg__page2;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:259.15-259.21
	wire<8> p_cnt__rd;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:258.15-258.21
	wire<7> p_cnt__wr;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:257.9-257.17
	wire<1> p_dbg__mode;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:256.9-256.17
	wire<1> p_st__fight;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:255.25-255.30
	wire<1> p_st__wd;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:255.9-255.16
	wire<1> p_st__berr;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:254.16-254.18
	wire<10> p_wd;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:251.16-251.19
	wire<4> p_eng;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:241.9-241.29
	wire<1> p_ctl__clr__sticky__pulse;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:238.16-238.24
	wire<2> p_gd__wr__sr;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:237.16-237.24
	wire<2> p_ga__hi__sr;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:236.16-236.24
	wire<2> p_ga__lo__sr;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:235.16-235.22
	wire<1> p_go__arm;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:234.16-234.30
	wire<1> p_csr__rd__pending;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:233.16-233.23
	wire<1> p_attr__a0;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:232.16-232.23
	wire<3> p_attr__fc;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:231.32-231.41
	wire<1> p_attr__byte;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:231.16-231.23
	wire<1> p_attr__rd;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:230.16-230.26
	wire<1> p_go__pending;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:225.28-225.41
	wire<1> p_st__bgack__seen;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:225.9-225.19
	wire<1> p_st__br__seen;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:221.15-221.18
	wire<2> p_arb;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:211.15-211.20
	wire<4> p_e__cnt;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:198.26-198.33
	wire<1> p_rd__prev;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:198.10-198.17
	wire<1> p_wr__prev;
	// \init: 7
	// \src: ../rtl/pistorm_psp2.v:181.33-181.39
	wire<3> p_s__ipl2;
	// \init: 7
	// \src: ../rtl/pistorm_psp2.v:181.15-181.21
	wire<3> p_s__ipl1;
	// \init: 3
	// \src: ../rtl/pistorm_psp2.v:180.32-180.39
	wire<2> p_s__bgack;
	// \init: 3
	// \src: ../rtl/pistorm_psp2.v:180.15-180.19
	wire<2> p_s__br;
	// \init: 3
	// \src: ../rtl/pistorm_psp2.v:179.49-179.54
	wire<2> p_s__vpa;
	// \init: 3
	// \src: ../rtl/pistorm_psp2.v:179.32-179.38
	wire<2> p_s__berr;
	// \init: 3
	// \src: ../rtl/pistorm_psp2.v:179.15-179.22
	wire<2> p_s__dtack;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:178.32-178.36
	wire<2> p_s__rd;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:178.15-178.19
	wire<2> p_s__wr;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:70.36-70.41
	wire<1> p_lds__q;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:70.22-70.27
	wire<1> p_uds__q;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:70.9-70.13
	wire<1> p_as__q;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:69.23-69.27
	wire<1> p_bg__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:69.9-69.14
	wire<1> p_vma__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:68.50-68.54
	wire<1> p_rw__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:68.36-68.41
	wire<1> p_lds__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:68.22-68.27
	wire<1> p_uds__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:68.9-68.13
	wire<1> p_as__i;
	// \src: ../rtl/pistorm_psp2.v:64.24-64.31
	/*input*/ value<1> p_bgack__n;
	// \src: ../rtl/pistorm_psp2.v:63.24-63.28
	/*output*/ value<1> p_bg__n;
	// \src: ../rtl/pistorm_psp2.v:62.24-62.28
	/*input*/ value<1> p_br__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:61.24-61.34
	/*output*/ wire<1> p_halt__drive;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:60.24-60.35
	/*output*/ wire<1> p_reset__drive;
	// \src: ../rtl/pistorm_psp2.v:59.24-59.34
	/*input*/ value<1> p_reset__n__in;
	// \src: ../rtl/pistorm_psp2.v:58.24-58.29
	/*input*/ value<3> p_ipl__n;
	// \src: ../rtl/pistorm_psp2.v:57.24-57.29
	/*output*/ value<1> p_vma__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:56.24-56.29
	/*output*/ wire<1> p_e__clk;
	// \src: ../rtl/pistorm_psp2.v:55.24-55.29
	/*input*/ value<1> p_vpa__n;
	// \src: ../rtl/pistorm_psp2.v:54.24-54.30
	/*input*/ value<1> p_berr__n;
	// \src: ../rtl/pistorm_psp2.v:53.24-53.31
	/*input*/ value<1> p_dtack__n;
	// \src: ../rtl/pistorm_psp2.v:52.24-52.26
	/*output*/ value<1> p_rw;
	// \src: ../rtl/pistorm_psp2.v:51.24-51.29
	/*output*/ value<1> p_lds__n;
	// \src: ../rtl/pistorm_psp2.v:50.24-50.29
	/*output*/ value<1> p_uds__n;
	// \src: ../rtl/pistorm_psp2.v:49.24-49.28
	/*output*/ value<1> p_as__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:48.24-48.30
	/*output*/ wire<1> p_bus__oe;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:47.24-47.30
	/*output*/ wire<3> p_fc__out;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:44.24-44.38
	/*output*/ wire<1> p_ltch__d__rd__oe__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:43.24-43.35
	/*output*/ wire<1> p_ltch__d__rd__l;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:42.24-42.35
	/*output*/ wire<1> p_ltch__d__rd__u;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:41.24-41.38
	/*output*/ wire<1> p_ltch__d__wr__oe__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:40.24-40.35
	/*output*/ wire<1> p_ltch__d__wr__l;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:39.24-39.35
	/*output*/ wire<1> p_ltch__d__wr__u;
	// \init: 1
	// \src: ../rtl/pistorm_psp2.v:38.24-38.35
	/*output*/ wire<1> p_ltch__a__oe__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:37.24-37.33
	/*output*/ wire<1> p_ltch__a__24;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:36.24-36.33
	/*output*/ wire<1> p_ltch__a__16;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:35.24-35.32
	/*output*/ wire<1> p_ltch__a__8;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:34.24-34.32
	/*output*/ wire<1> p_ltch__a__0;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:31.24-31.31
	/*output*/ wire<1> p_pi__ipl2;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:30.24-30.31
	/*output*/ wire<1> p_pi__ipl1;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:29.24-29.31
	/*output*/ wire<1> p_pi__berr;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:27.24-27.28
	/*output*/ wire<1> p_busy;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:26.24-26.31
	/*output*/ wire<1> p_pi__d__oe;
	// \init: 0
	// \src: ../rtl/pistorm_psp2.v:25.24-25.32
	/*output*/ wire<16> p_pi__d__out;
	// \src: ../rtl/pistorm_psp2.v:24.24-24.31
	/*input*/ value<16> p_pi__d__in;
	// \src: ../rtl/pistorm_psp2.v:23.24-23.30
	/*input*/ value<2> p_pi__cmd;
	// \src: ../rtl/pistorm_psp2.v:22.24-22.29
	/*input*/ value<1> p_pi__rd;
	// \src: ../rtl/pistorm_psp2.v:21.24-21.29
	/*input*/ value<1> p_pi__wr;
	// \src: ../rtl/pistorm_psp2.v:18.24-18.27
	/*input*/ value<1> p_clk;
	value<1> prev_p_clk;
	bool posedge_p_clk() const {
		return !prev_p_clk.slice<0>().val() && p_clk.slice<0>().val();
	}
	bool negedge_p_clk() const {
		return prev_p_clk.slice<0>().val() && !p_clk.slice<0>().val();
	}
	// \src: ../rtl/pistorm_psp2.v:273.15-273.18
	/*outline*/ value<1> p_por;
	// \src: ../rtl/pistorm_psp2.v:223.10-223.18
	/*outline*/ value<1> p_ext__owns;
	// \src: ../rtl/pistorm_psp2.v:222.10-222.21
	/*outline*/ value<1> p_engine__idle;
	// \src: ../rtl/pistorm_psp2.v:206.10-206.17
	/*outline*/ value<1> p_bgack__a;
	// \src: ../rtl/pistorm_psp2.v:205.10-205.14
	/*outline*/ value<1> p_br__a;
	// \src: ../rtl/pistorm_psp2.v:203.10-203.16
	/*outline*/ value<1> p_berr__a;
	// \src: ../rtl/pistorm_psp2.v:200.10-200.17
	/*outline*/ value<1> p_rd__edge;
	// \src: ../rtl/pistorm_psp2.v:199.10-199.17
	/*outline*/ value<1> p_wr__edge;
	// \src: ../rtl/pistorm_psp2.v:197.10-197.17
	/*outline*/ value<1> p_rd__sync;
	// \src: ../rtl/pistorm_psp2.v:196.10-196.17
	/*outline*/ value<1> p_wr__sync;
	p_pistorm__psp2__core(interior) {}
	p_pistorm__psp2__core() {
		reset();
	};

	void reset() override;

	bool eval(performer *performer = nullptr) override;

	template<class ObserverT>
	bool commit(ObserverT &observer) {
		bool changed = false;
		if (p_por__cnt.commit(observer)) changed = true;
		if (p_bgack__prev.commit(observer)) changed = true;
		if (p_br__prev.commit(observer)) changed = true;
		if (p_cnt__bgack.commit(observer)) changed = true;
		if (p_cnt__br.commit(observer)) changed = true;
		if (p_dbg__page2.commit(observer)) changed = true;
		if (p_cnt__rd.commit(observer)) changed = true;
		if (p_cnt__wr.commit(observer)) changed = true;
		if (p_dbg__mode.commit(observer)) changed = true;
		if (p_st__fight.commit(observer)) changed = true;
		if (p_st__wd.commit(observer)) changed = true;
		if (p_st__berr.commit(observer)) changed = true;
		if (p_wd.commit(observer)) changed = true;
		if (p_eng.commit(observer)) changed = true;
		if (p_ctl__clr__sticky__pulse.commit(observer)) changed = true;
		if (p_gd__wr__sr.commit(observer)) changed = true;
		if (p_ga__hi__sr.commit(observer)) changed = true;
		if (p_ga__lo__sr.commit(observer)) changed = true;
		if (p_go__arm.commit(observer)) changed = true;
		if (p_csr__rd__pending.commit(observer)) changed = true;
		if (p_attr__a0.commit(observer)) changed = true;
		if (p_attr__fc.commit(observer)) changed = true;
		if (p_attr__byte.commit(observer)) changed = true;
		if (p_attr__rd.commit(observer)) changed = true;
		if (p_go__pending.commit(observer)) changed = true;
		if (p_st__bgack__seen.commit(observer)) changed = true;
		if (p_st__br__seen.commit(observer)) changed = true;
		if (p_arb.commit(observer)) changed = true;
		if (p_e__cnt.commit(observer)) changed = true;
		if (p_rd__prev.commit(observer)) changed = true;
		if (p_wr__prev.commit(observer)) changed = true;
		if (p_s__ipl2.commit(observer)) changed = true;
		if (p_s__ipl1.commit(observer)) changed = true;
		if (p_s__bgack.commit(observer)) changed = true;
		if (p_s__br.commit(observer)) changed = true;
		if (p_s__vpa.commit(observer)) changed = true;
		if (p_s__berr.commit(observer)) changed = true;
		if (p_s__dtack.commit(observer)) changed = true;
		if (p_s__rd.commit(observer)) changed = true;
		if (p_s__wr.commit(observer)) changed = true;
		if (p_lds__q.commit(observer)) changed = true;
		if (p_uds__q.commit(observer)) changed = true;
		if (p_as__q.commit(observer)) changed = true;
		if (p_bg__i.commit(observer)) changed = true;
		if (p_vma__i.commit(observer)) changed = true;
		if (p_rw__i.commit(observer)) changed = true;
		if (p_lds__i.commit(observer)) changed = true;
		if (p_uds__i.commit(observer)) changed = true;
		if (p_as__i.commit(observer)) changed = true;
		if (p_halt__drive.commit(observer)) changed = true;
		if (p_reset__drive.commit(observer)) changed = true;
		if (p_e__clk.commit(observer)) changed = true;
		if (p_bus__oe.commit(observer)) changed = true;
		if (p_fc__out.commit(observer)) changed = true;
		if (p_ltch__d__rd__oe__n.commit(observer)) changed = true;
		if (p_ltch__d__rd__l.commit(observer)) changed = true;
		if (p_ltch__d__rd__u.commit(observer)) changed = true;
		if (p_ltch__d__wr__oe__n.commit(observer)) changed = true;
		if (p_ltch__d__wr__l.commit(observer)) changed = true;
		if (p_ltch__d__wr__u.commit(observer)) changed = true;
		if (p_ltch__a__oe__n.commit(observer)) changed = true;
		if (p_ltch__a__24.commit(observer)) changed = true;
		if (p_ltch__a__16.commit(observer)) changed = true;
		if (p_ltch__a__8.commit(observer)) changed = true;
		if (p_ltch__a__0.commit(observer)) changed = true;
		if (p_pi__ipl2.commit(observer)) changed = true;
		if (p_pi__ipl1.commit(observer)) changed = true;
		if (p_pi__berr.commit(observer)) changed = true;
		if (p_busy.commit(observer)) changed = true;
		if (p_pi__d__oe.commit(observer)) changed = true;
		if (p_pi__d__out.commit(observer)) changed = true;
		prev_p_clk = p_clk;
		return changed;
	}

	bool commit() override {
		observer observer;
		return commit<>(observer);
	}

	void debug_eval();
	debug_outline debug_eval_outline { std::bind(&p_pistorm__psp2__core::debug_eval, this) };

	void debug_info(debug_items *items, debug_scopes *scopes, std::string path, metadata_map &&cell_attrs = {}) override;
}; // struct p_pistorm__psp2__core

void p_pistorm__psp2__core::reset() {
	p_por__cnt = wire<3>{0u};
	p_bgack__prev = wire<1>{0u};
	p_br__prev = wire<1>{0u};
	p_cnt__bgack = wire<8>{0u};
	p_cnt__br = wire<8>{0u};
	p_dbg__page2 = wire<1>{0u};
	p_cnt__rd = wire<8>{0u};
	p_cnt__wr = wire<7>{0u};
	p_dbg__mode = wire<1>{0u};
	p_st__fight = wire<1>{0u};
	p_st__wd = wire<1>{0u};
	p_st__berr = wire<1>{0u};
	p_wd = wire<10>{0u};
	p_eng = wire<4>{0u};
	p_ctl__clr__sticky__pulse = wire<1>{0u};
	p_gd__wr__sr = wire<2>{0u};
	p_ga__hi__sr = wire<2>{0u};
	p_ga__lo__sr = wire<2>{0u};
	p_go__arm = wire<1>{0u};
	p_csr__rd__pending = wire<1>{0u};
	p_attr__a0 = wire<1>{0u};
	p_attr__fc = wire<3>{0u};
	p_attr__byte = wire<1>{0u};
	p_attr__rd = wire<1>{0u};
	p_go__pending = wire<1>{0u};
	p_st__bgack__seen = wire<1>{0u};
	p_st__br__seen = wire<1>{0u};
	p_arb = wire<2>{0u};
	p_e__cnt = wire<4>{0u};
	p_rd__prev = wire<1>{0u};
	p_wr__prev = wire<1>{0u};
	p_s__ipl2 = wire<3>{0x7u};
	p_s__ipl1 = wire<3>{0x7u};
	p_s__bgack = wire<2>{0x3u};
	p_s__br = wire<2>{0x3u};
	p_s__vpa = wire<2>{0x3u};
	p_s__berr = wire<2>{0x3u};
	p_s__dtack = wire<2>{0x3u};
	p_s__rd = wire<2>{0u};
	p_s__wr = wire<2>{0u};
	p_lds__q = wire<1>{0x1u};
	p_uds__q = wire<1>{0x1u};
	p_as__q = wire<1>{0x1u};
	p_bg__i = wire<1>{0x1u};
	p_vma__i = wire<1>{0x1u};
	p_rw__i = wire<1>{0x1u};
	p_lds__i = wire<1>{0x1u};
	p_uds__i = wire<1>{0x1u};
	p_as__i = wire<1>{0x1u};
	p_halt__drive = wire<1>{0u};
	p_reset__drive = wire<1>{0u};
	p_e__clk = wire<1>{0u};
	p_bus__oe = wire<1>{0u};
	p_fc__out = wire<3>{0u};
	p_ltch__d__rd__oe__n = wire<1>{0x1u};
	p_ltch__d__rd__l = wire<1>{0u};
	p_ltch__d__rd__u = wire<1>{0u};
	p_ltch__d__wr__oe__n = wire<1>{0x1u};
	p_ltch__d__wr__l = wire<1>{0u};
	p_ltch__d__wr__u = wire<1>{0u};
	p_ltch__a__oe__n = wire<1>{0x1u};
	p_ltch__a__24 = wire<1>{0u};
	p_ltch__a__16 = wire<1>{0u};
	p_ltch__a__8 = wire<1>{0u};
	p_ltch__a__0 = wire<1>{0u};
	p_pi__ipl2 = wire<1>{0u};
	p_pi__ipl1 = wire<1>{0u};
	p_pi__berr = wire<1>{0u};
	p_busy = wire<1>{0u};
	p_pi__d__oe = wire<1>{0u};
	p_pi__d__out = wire<16>{0u};
}

bool p_pistorm__psp2__core::eval(performer *performer) {
	bool converged = true;
	bool posedge_p_clk = this->posedge_p_clk();
	bool negedge_p_clk = this->negedge_p_clk();
	value<1> i_procmux_24_389__Y;
	value<4> i_procmux_24_334__Y;
	value<1> i_procmux_24_442__Y;
	value<1> i_procmux_24_640__Y;
	value<1> i_procmux_24_645__Y;
	// \src: ../rtl/pistorm_psp2.v:429.21-429.36
	value<1> i_logic__or_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_429_24_62__Y;
	// \src: ../rtl/pistorm_psp2.v:400.26-400.61
	value<1> i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y;
	value<1> i_procmux_24_305__Y;
	// \src: ../rtl/pistorm_psp2.v:372.26-372.45
	value<1> i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_372_24_41__Y;
	// \src: ../rtl/pistorm_psp2.v:288.25-288.42
	value<1> i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_288_24_29__Y;
	// \src: ../rtl/pistorm_psp2.v:287.25-287.42
	value<1> i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_287_24_28__Y;
	// \src: ../rtl/pistorm_psp2.v:286.58-286.75
	value<1> i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_286_24_27__Y;
	// \src: ../rtl/pistorm_psp2.v:286.23-286.40
	value<1> i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_286_24_26__Y;
	// \src: ../rtl/pistorm_psp2.v:285.58-285.75
	value<1> i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_285_24_25__Y;
	// \src: ../rtl/pistorm_psp2.v:285.23-285.40
	value<1> i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_285_24_24__Y;
	value<1> i_procmux_24_295__Y;
	// \src: ../rtl/pistorm_psp2.v:273.15-273.18
	value<1> p_por;
	// \src: ../rtl/pistorm_psp2.v:222.10-222.21
	value<1> p_engine__idle;
	// \src: ../rtl/pistorm_psp2.v:206.10-206.17
	value<1> p_bgack__a;
	// \src: ../rtl/pistorm_psp2.v:205.10-205.14
	value<1> p_br__a;
	// \src: ../rtl/pistorm_psp2.v:203.10-203.16
	value<1> p_berr__a;
	// \src: ../rtl/pistorm_psp2.v:200.10-200.17
	value<1> p_rd__edge;
	// \src: ../rtl/pistorm_psp2.v:199.10-199.17
	value<1> p_wr__edge;
	// cells $and$../rtl/pistorm_psp2.v:199$7 $not$../rtl/pistorm_psp2.v:199$6
	p_wr__edge = and_uu<1>(p_s__wr.curr.slice<1>().val(), not_u<1>(p_wr__prev.curr));
	// cells $procmux$640 $procmux$638 $procmux$639_CMP0 $procmux$636
	i_procmux_24_640__Y = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? (p_pi__d__in.slice<0>().val() ? value<1>{0x1u} : p_ltch__d__rd__oe__n.curr) : p_ltch__d__rd__oe__n.curr) : p_ltch__d__rd__oe__n.curr);
	// cells $procmux$334 $procmux$332 $procmux$333_CMP0 $procmux$330
	i_procmux_24_334__Y = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? (p_pi__d__in.slice<0>().val() ? value<4>{0u} : p_eng.curr) : p_eng.curr) : p_eng.curr);
	// \src: ../rtl/pistorm_psp2.v:252.27-252.40
	// cell $eq$../rtl/pistorm_psp2.v:252$21
	p_engine__idle = logic_not<1>(p_eng.curr);
	// cells $logic_and$../rtl/pistorm_psp2.v:400$50 $logic_and$../rtl/pistorm_psp2.v:400$48 $eq$../rtl/pistorm_psp2.v:400$47
	i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y = logic_and<1>(logic_and<1>(p_go__pending.curr, logic_not<1>(p_arb.curr)), p_s__br.curr.slice<1>().val());
	// cells $and$../rtl/pistorm_psp2.v:200$9 $not$../rtl/pistorm_psp2.v:200$8
	p_rd__edge = and_uu<1>(p_s__rd.curr.slice<1>().val(), not_u<1>(p_rd__prev.curr));
	// \src: ../rtl/pistorm_psp2.v:203.20-203.30
	// cell $not$../rtl/pistorm_psp2.v:203$11
	p_berr__a = not_u<1>(p_s__berr.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp2.v:205.20-205.28
	// cell $not$../rtl/pistorm_psp2.v:205$13
	p_br__a = not_u<1>(p_s__br.curr.slice<1>().val());
	// cells $procmux$442 $procmux$440 $procmux$438 $procmux$439_CMP0 $procmux$436
	i_procmux_24_442__Y = (p_go__arm.curr ? value<1>{0x1u} : (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? (p_pi__d__in.slice<0>().val() ? value<1>{0u} : p_go__pending.curr) : p_go__pending.curr) : p_go__pending.curr));
	// cells $procmux$645 $procmux$643 $procmux$644_CMP0
	i_procmux_24_645__Y = (p_rd__edge ? (logic_not<1>(p_pi__cmd) ? value<1>{0x1u} : i_procmux_24_640__Y) : i_procmux_24_640__Y);
	// cells $logic_or$../rtl/pistorm_psp2.v:429$62 $reduce_and$../rtl/pistorm_psp2.v:429$61
	i_logic__or_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_429_24_62__Y = logic_or<1>(p_berr__a, reduce_and<1>(p_wd.curr));
	// \src: ../rtl/pistorm_psp2.v:372.26-372.45
	// cell $logic_and$../rtl/pistorm_psp2.v:372$41
	i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_372_24_41__Y = logic_and<1>(p_br__a, p_engine__idle);
	// cells $procmux$389 $procmux$387 $procmux$388_CMP0 $procmux$385 $eq$../rtl/pistorm_psp2.v:338$34
	i_procmux_24_389__Y = (p_rd__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? (logic_not<1>(p_eng.curr) ? value<1>{0x1u} : p_csr__rd__pending.curr) : p_csr__rd__pending.curr) : p_csr__rd__pending.curr);
	// \src: ../rtl/pistorm_psp2.v:364.13-364.33|../rtl/pistorm_psp2.v:364.9-368.12
	// cell $procmux$305
	i_procmux_24_305__Y = (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : p_st__berr.curr);
	// \src: ../rtl/pistorm_psp2.v:364.13-364.33|../rtl/pistorm_psp2.v:364.9-368.12
	// cell $procmux$295
	i_procmux_24_295__Y = (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : p_st__wd.curr);
	// \src: ../rtl/pistorm_psp2.v:285.23-285.40
	// cell $ne$../rtl/pistorm_psp2.v:285$24
	i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_285_24_24__Y = reduce_bool<1>(p_ga__lo__sr.curr);
	// \src: ../rtl/pistorm_psp2.v:285.58-285.75
	// cell $ne$../rtl/pistorm_psp2.v:285$25
	i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_285_24_25__Y = reduce_bool<1>(p_ga__lo__sr.curr);
	// \src: ../rtl/pistorm_psp2.v:286.23-286.40
	// cell $ne$../rtl/pistorm_psp2.v:286$26
	i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_286_24_26__Y = reduce_bool<1>(p_ga__hi__sr.curr);
	// \src: ../rtl/pistorm_psp2.v:286.58-286.75
	// cell $ne$../rtl/pistorm_psp2.v:286$27
	i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_286_24_27__Y = reduce_bool<1>(p_ga__hi__sr.curr);
	// \src: ../rtl/pistorm_psp2.v:287.25-287.42
	// cell $ne$../rtl/pistorm_psp2.v:287$28
	i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_287_24_28__Y = reduce_bool<1>(p_gd__wr__sr.curr);
	// \src: ../rtl/pistorm_psp2.v:288.25-288.42
	// cell $ne$../rtl/pistorm_psp2.v:288$29
	i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_288_24_29__Y = reduce_bool<1>(p_gd__wr__sr.curr);
	// \src: ../rtl/pistorm_psp2.v:206.20-206.31
	// cell $not$../rtl/pistorm_psp2.v:206$14
	p_bgack__a = not_u<1>(p_s__bgack.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp2.v:273.22-273.37
	// cell $ne$../rtl/pistorm_psp2.v:273$22
	p_por = ne_uu<1>(p_por__cnt.curr, value<3>{0x7u});
	// \src: ../rtl/pistorm_psp2.v:76.20-76.32
	// cell $and$../rtl/pistorm_psp2.v:76$2
	p_as__n = and_uu<1>(p_as__i.curr, p_as__q.curr);
	// \src: ../rtl/pistorm_psp2.v:77.20-77.33
	// cell $and$../rtl/pistorm_psp2.v:77$3
	p_uds__n = and_uu<1>(p_uds__i.curr, p_uds__q.curr);
	// \src: ../rtl/pistorm_psp2.v:78.20-78.33
	// cell $and$../rtl/pistorm_psp2.v:78$4
	p_lds__n = and_uu<1>(p_lds__i.curr, p_lds__q.curr);
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$782
	if (posedge_p_clk) {
		p_s__ipl2.next = p_s__ipl2.curr.slice<1,0>().concat(p_ipl__n.slice<2>()).val();
	}
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$781
	if (posedge_p_clk) {
		p_s__ipl1.next = p_s__ipl1.curr.slice<1,0>().concat(p_ipl__n.slice<1>()).val();
	}
	// \src: ../rtl/pistorm_psp2.v:71.5-73.8
	// cell $procdff$785
	if (negedge_p_clk) {
		p_lds__q.next = p_lds__i.curr;
	}
	// \src: ../rtl/pistorm_psp2.v:71.5-73.8
	// cell $procdff$784
	if (negedge_p_clk) {
		p_uds__q.next = p_uds__i.curr;
	}
	// \src: ../rtl/pistorm_psp2.v:71.5-73.8
	// cell $procdff$783
	if (negedge_p_clk) {
		p_as__q.next = p_as__i.curr;
	}
	// cells $procdff$714 $procmux$712 $procmux$713_CMP0 $ternary$../rtl/pistorm_psp2.v:479$75 $ternary$../rtl/pistorm_psp2.v:480$74 $eq$../rtl/pistorm_psp2.v:223$20 $not$../rtl/pistorm_psp2.v:482$71 $not$../rtl/pistorm_psp2.v:482$72 $not$../rtl/pistorm_psp2.v:482$73
	if (posedge_p_clk) {
		p_pi__d__out.next = (eq_uu<1>(p_eng.curr, value<4>{0xcu}) ? (p_dbg__page2.curr ? p_cnt__br.curr.concat(p_cnt__bgack.curr).val() : (p_dbg__mode.curr ? p_st__fight.curr.concat(p_cnt__wr.curr).concat(p_cnt__rd.curr).val() : not_u<1>(p_engine__idle).concat(not_u<1>(p_s__berr.curr.slice<1>().val())).concat(not_u<1>(p_s__dtack.curr.slice<1>().val())).concat(eq_uu<1>(p_arb.curr, value<2>{0x2u})).concat(p_st__br__seen.curr).concat(p_st__bgack__seen.curr).concat(p_st__wd.curr).concat(p_st__berr.curr).concat(value<8>{0x2au}).val())) : p_pi__d__out.curr);
	}
	// cells $procdff$715 $procmux$708 $procmux$706 $logic_and$../rtl/pistorm_psp2.v:501$77 $eq$../rtl/pistorm_psp2.v:501$76 $procmux$704 $procmux$705_CMP0 $procmux$700 $procmux$698 $procmux$699_CMP0 $procmux$696
	if (posedge_p_clk) {
		p_pi__d__oe.next = (p_por ? value<1>{0u} : (logic_and<1>(p_rd__edge, logic_not<1>(p_pi__cmd)) ? value<1>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xcu}) ? value<1>{0x1u} : (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? (p_pi__d__in.slice<0>().val() ? value<1>{0u} : p_pi__d__oe.curr) : p_pi__d__oe.curr) : p_pi__d__oe.curr))));
	}
	// cells $procdff$716 $procmux$690 $procmux$691_CMP0 $procmux$695_CMP0 $not$../rtl/pistorm_psp2.v:474$70 $procmux$693 $not$../rtl/pistorm_psp2.v:466$69
	if (posedge_p_clk) {
		p_busy.next = (eq_uu<1>(p_eng.curr, value<4>{0x9u}) ? not_u<1>(p_busy.curr) : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? (p_attr__rd.curr ? p_busy.curr : not_u<1>(p_busy.curr)) : p_busy.curr));
	}
	// cells $procdff$717 $procmux$685 $procmux$683 $procmux$684_CMP0 $procmux$680
	if (posedge_p_clk) {
		p_pi__berr.next = (p_por ? value<1>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xbu}) ? value<1>{0x1u} : (p_go__arm.curr ? value<1>{0u} : p_pi__berr.curr)));
	}
	// cells $procdff$718 $not$../rtl/pistorm_psp2.v:290$30
	if (posedge_p_clk) {
		p_pi__ipl1.next = not_u<1>(p_s__ipl1.curr.slice<2>().val());
	}
	// cells $procdff$719 $not$../rtl/pistorm_psp2.v:291$31
	if (posedge_p_clk) {
		p_pi__ipl2.next = not_u<1>(p_s__ipl2.curr.slice<2>().val());
	}
	// cells $procdff$720 $procmux$183 $procmux$181 $procmux$182_CMP0
	if (posedge_p_clk) {
		p_ltch__a__0.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x1u}) ? value<1>{0x1u} : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_285_24_24__Y) : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_285_24_24__Y);
	}
	// cells $procdff$721 $procmux$176 $procmux$174 $procmux$175_CMP0
	if (posedge_p_clk) {
		p_ltch__a__8.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x1u}) ? value<1>{0x1u} : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_285_24_25__Y) : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_285_24_25__Y);
	}
	// cells $procdff$722 $procmux$169 $procmux$167 $procmux$168_CMP0
	if (posedge_p_clk) {
		p_ltch__a__16.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x2u}) ? value<1>{0x1u} : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_286_24_26__Y) : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_286_24_26__Y);
	}
	// cells $procdff$723 $procmux$163 $procmux$161 $procmux$162_CMP0
	if (posedge_p_clk) {
		p_ltch__a__24.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x2u}) ? value<1>{0x1u} : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_286_24_27__Y) : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_286_24_27__Y);
	}
	// cells $procdff$724 $procmux$139 $ternary$../rtl/pistorm_psp2.v:388$46 $eq$../rtl/pistorm_psp2.v:388$45
	if (posedge_p_clk) {
		p_ltch__a__oe__n.next = (p_por ? value<1>{0x1u} : (logic_not<1>(p_arb.curr) ? value<1>{0u} : value<1>{0x1u}));
	}
	// cells $procdff$725 $procmux$157 $procmux$155 $procmux$156_CMP0
	if (posedge_p_clk) {
		p_ltch__d__wr__u.next = (p_wr__edge ? (logic_not<1>(p_pi__cmd) ? value<1>{0x1u} : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_287_24_28__Y) : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_287_24_28__Y);
	}
	// cells $procdff$726 $procmux$152 $procmux$150 $procmux$151_CMP0
	if (posedge_p_clk) {
		p_ltch__d__wr__l.next = (p_wr__edge ? (logic_not<1>(p_pi__cmd) ? value<1>{0x1u} : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_288_24_29__Y) : i_ne_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_288_24_29__Y);
	}
	// cells $procdff$727 $procmux$678 $procmux$666 $procmux$667_CMP0 $procmux$668_CMP0 $procmux$677_CMP0 $procmux$675 $procmux$672 $procmux$670
	if (posedge_p_clk) {
		p_ltch__d__wr__oe__n.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x1u}) ? value<1>{0u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? p_ltch__d__wr__oe__n.curr : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? (p_attr__rd.curr ? p_ltch__d__wr__oe__n.curr : value<1>{0u}) : p_ltch__d__wr__oe__n.curr)) : p_ltch__d__wr__oe__n.curr))));
	}
	// cells $procdff$728 $procmux$227 $procmux$228_CMP0
	if (posedge_p_clk) {
		p_ltch__d__rd__u.next = (eq_uu<1>(p_eng.curr, value<4>{0x4u}) ? value<1>{0x1u} : value<1>{0u});
	}
	// cells $procdff$729 $procmux$217 $procmux$218_CMP0
	if (posedge_p_clk) {
		p_ltch__d__rd__l.next = (eq_uu<1>(p_eng.curr, value<4>{0x4u}) ? value<1>{0x1u} : value<1>{0u});
	}
	// cells $procdff$730 $procmux$658 $procmux$656 $procmux$657_CMP0 $procmux$654
	if (posedge_p_clk) {
		p_ltch__d__rd__oe__n.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? (p_attr__rd.curr ? value<1>{0u} : i_procmux_24_645__Y) : i_procmux_24_645__Y));
	}
	// cells $procdff$731 $procmux$634 $procmux$635_CMP0 $procmux$632 $procmux$629
	if (posedge_p_clk) {
		p_fc__out.next = (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? p_fc__out.curr : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? p_attr__fc.curr : p_fc__out.curr)) : p_fc__out.curr);
	}
	// cells $procdff$732 $procmux$141 $eq$../rtl/pistorm_psp2.v:387$44
	if (posedge_p_clk) {
		p_bus__oe.next = (p_por ? value<1>{0u} : logic_not<1>(p_arb.curr));
	}
	// cells $procdff$733 $procmux$614 $procmux$612 $procmux$610 $procmux$611_CMP0
	if (posedge_p_clk) {
		p_reset__drive.next = (p_por ? value<1>{0u} : (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? p_pi__d__in.slice<1>().val() : p_reset__drive.curr) : p_reset__drive.curr));
	}
	// cells $procdff$734 $procmux$608 $procmux$606 $procmux$604 $procmux$605_CMP0
	if (posedge_p_clk) {
		p_halt__drive.next = (p_por ? value<1>{0u} : (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? p_pi__d__in.slice<2>().val() : p_halt__drive.curr) : p_halt__drive.curr));
	}
	// cells $procdff$735 $procmux$602 $procmux$594 $procmux$595_CMP0 $procmux$601_CMP0 $procmux$599 $procmux$596
	if (posedge_p_clk) {
		p_as__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<1>{0x1u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0x1u} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? value<1>{0u} : value<1>{0x1u})) : p_as__i.curr)));
	}
	// cells $procdff$736 $procmux$585 $procmux$573 $procmux$574_CMP0 $procmux$575_CMP0 $procmux$584_CMP0 $ternary$../rtl/pistorm_psp2.v:422$57 $procmux$582 $procmux$579 $procmux$577 $ternary$../rtl/pistorm_psp2.v:408$53
	if (posedge_p_clk) {
		p_uds__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x2u}) ? (p_attr__byte.curr ? p_attr__a0.curr : value<1>{0u}) : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0x1u} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? (p_attr__rd.curr ? (p_attr__byte.curr ? p_attr__a0.curr : value<1>{0u}) : value<1>{0x1u}) : value<1>{0x1u})) : p_uds__i.curr))));
	}
	// cells $procdff$737 $procmux$564 $procmux$552 $procmux$553_CMP0 $procmux$554_CMP0 $procmux$563_CMP0 $ternary$../rtl/pistorm_psp2.v:423$59 $not$../rtl/pistorm_psp2.v:423$58 $procmux$561 $procmux$558 $procmux$556 $ternary$../rtl/pistorm_psp2.v:409$55 $not$../rtl/pistorm_psp2.v:409$54
	if (posedge_p_clk) {
		p_lds__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x2u}) ? (p_attr__byte.curr ? not_u<1>(p_attr__a0.curr) : value<1>{0u}) : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0x1u} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? (p_attr__rd.curr ? (p_attr__byte.curr ? not_u<1>(p_attr__a0.curr) : value<1>{0u}) : value<1>{0x1u}) : value<1>{0x1u})) : p_lds__i.curr))));
	}
	// cells $procdff$738 $procmux$543 $procmux$535 $procmux$536_CMP0 $procmux$542_CMP0 $procmux$540 $procmux$537
	if (posedge_p_clk) {
		p_rw__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? value<1>{0x1u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0x1u} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? p_attr__rd.curr : value<1>{0x1u})) : p_rw__i.curr)));
	}
	// cells $procdff$739 $procmux$527 $procmux$515 $procmux$516_CMP0 $procmux$525_CMP0 $procmux$526_CMP0 $procmux$523 $procmux$520 $procmux$517
	if (posedge_p_clk) {
		p_vma__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x3u}) ? (i_logic__or_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_429_24_62__Y ? p_vma__i.curr : (p_s__dtack.curr.slice<1>().val() ? (p_s__vpa.curr.slice<1>().val() ? p_vma__i.curr : value<1>{0u}) : p_vma__i.curr)) : (logic_not<1>(p_eng.curr) ? value<1>{0x1u} : p_vma__i.curr))));
	}
	// cells $procdff$740 $procmux$506 $procmux$501 $procmux$502_CMP0 $procmux$505_CMP0 $procmux$499 $procmux$496 $procmux$503
	if (posedge_p_clk) {
		p_bg__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_arb.curr, value<2>{0x1u}) ? (p_s__bgack.curr.slice<1>().val() ? (p_s__br.curr.slice<1>().val() ? value<1>{0x1u} : p_bg__i.curr) : value<1>{0x1u}) : (logic_not<1>(p_arb.curr) ? (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_372_24_41__Y ? value<1>{0u} : p_bg__i.curr) : p_bg__i.curr)));
	}
	// \src: ../rtl/pistorm_psp2.v:275.5-524.8
	// cell $procdff$741
	if (posedge_p_clk) {
		p_wr__prev.next = p_s__wr.curr.slice<1>().val();
	}
	// \src: ../rtl/pistorm_psp2.v:275.5-524.8
	// cell $procdff$742
	if (posedge_p_clk) {
		p_rd__prev.next = p_s__rd.curr.slice<1>().val();
	}
	// cells $procdff$743 $procmux$492 $procmux$478 $procmux$479_CMP0 $procmux$482_CMP0 $procmux$488_CMP0 $procmux$491_CMP0 $procmux$480 $procmux$486 $procmux$483 $procmux$489
	if (posedge_p_clk) {
		p_arb.next = (p_por ? value<2>{0u} : (eq_uu<1>(p_arb.curr, value<2>{0x3u}) ? value<2>{0u} : (eq_uu<1>(p_arb.curr, value<2>{0x2u}) ? (p_s__bgack.curr.slice<1>().val() ? value<2>{0x3u} : p_arb.curr) : (eq_uu<1>(p_arb.curr, value<2>{0x1u}) ? (p_s__bgack.curr.slice<1>().val() ? (p_s__br.curr.slice<1>().val() ? value<2>{0x3u} : p_arb.curr) : value<2>{0x2u}) : (logic_not<1>(p_arb.curr) ? (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_372_24_41__Y ? value<2>{0x1u} : p_arb.curr) : p_arb.curr)))));
	}
	// cells $procdff$744 $procmux$476 $procmux$474 $procmux$472
	if (posedge_p_clk) {
		p_st__br__seen.next = (p_por ? value<1>{0u} : (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : (p_s__br.curr.slice<1>().val() ? p_st__br__seen.curr : value<1>{0x1u})));
	}
	// cells $procdff$745 $procmux$470 $procmux$468 $procmux$466
	if (posedge_p_clk) {
		p_st__bgack__seen.next = (p_por ? value<1>{0u} : (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : (p_s__bgack.curr.slice<1>().val() ? p_st__bgack__seen.curr : value<1>{0x1u})));
	}
	// cells $procdff$746 $procmux$464 $procmux$462 $procmux$463_CMP0 $procmux$460 $procmux$457
	if (posedge_p_clk) {
		p_go__pending.next = (p_por ? value<1>{0u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? i_procmux_24_442__Y : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? value<1>{0u} : i_procmux_24_442__Y)) : i_procmux_24_442__Y));
	}
	// cells $procdff$747 $procmux$434 $procmux$432 $procmux$433_CMP0
	if (posedge_p_clk) {
		p_attr__rd.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x2u}) ? p_pi__d__in.slice<12>().val() : p_attr__rd.curr) : p_attr__rd.curr);
	}
	// cells $procdff$748 $procmux$428 $procmux$426 $procmux$427_CMP0
	if (posedge_p_clk) {
		p_attr__byte.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x2u}) ? p_pi__d__in.slice<11>().val() : p_attr__byte.curr) : p_attr__byte.curr);
	}
	// cells $procdff$749 $procmux$422 $procmux$420 $procmux$421_CMP0
	if (posedge_p_clk) {
		p_attr__fc.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x2u}) ? p_pi__d__in.slice<15,13>().val() : p_attr__fc.curr) : p_attr__fc.curr);
	}
	// cells $procdff$750 $procmux$416 $procmux$414 $procmux$415_CMP0
	if (posedge_p_clk) {
		p_attr__a0.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x1u}) ? p_pi__d__in.slice<0>().val() : p_attr__a0.curr) : p_attr__a0.curr);
	}
	// cells $procdff$751 $procmux$409 $procmux$407 $procmux$408_CMP0 $procmux$405
	if (posedge_p_clk) {
		p_csr__rd__pending.next = (p_por ? value<1>{0u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0u} : i_procmux_24_389__Y) : i_procmux_24_389__Y));
	}
	// cells $procdff$752 $procmux$383 $procmux$381 $procmux$379 $procmux$374 $procmux$375_CMP0 $procmux$378_CMP0 $procmux$376
	if (posedge_p_clk) {
		p_go__arm.next = (p_por ? value<1>{0u} : (p_go__arm.curr ? value<1>{0u} : (p_wr__edge ? (logic_not<1>(p_pi__cmd) ? value<1>{0x1u} : (eq_uu<1>(p_pi__cmd, value<2>{0x2u}) ? (p_pi__d__in.slice<12>().val() ? value<1>{0x1u} : p_go__arm.curr) : p_go__arm.curr)) : p_go__arm.curr)));
	}
	// cells $procdff$753 $procmux$207 $procmux$205 $procmux$203 $procmux$204_CMP0
	if (posedge_p_clk) {
		p_ga__lo__sr.next = (p_por ? value<2>{0u} : (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x1u}) ? value<2>{0x3u} : value<1>{0u}.concat(p_ga__lo__sr.curr.slice<1>()).val()) : value<1>{0u}.concat(p_ga__lo__sr.curr.slice<1>()).val()));
	}
	// cells $procdff$754 $procmux$198 $procmux$196 $procmux$194 $procmux$195_CMP0
	if (posedge_p_clk) {
		p_ga__hi__sr.next = (p_por ? value<2>{0u} : (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x2u}) ? value<2>{0x3u} : value<1>{0u}.concat(p_ga__hi__sr.curr.slice<1>()).val()) : value<1>{0u}.concat(p_ga__hi__sr.curr.slice<1>()).val()));
	}
	// cells $procdff$755 $procmux$190 $procmux$188 $procmux$186 $procmux$187_CMP0
	if (posedge_p_clk) {
		p_gd__wr__sr.next = (p_por ? value<2>{0u} : (p_wr__edge ? (logic_not<1>(p_pi__cmd) ? value<2>{0x3u} : value<1>{0u}.concat(p_gd__wr__sr.curr.slice<1>()).val()) : value<1>{0u}.concat(p_gd__wr__sr.curr.slice<1>()).val()));
	}
	// cells $procdff$756 $procmux$147 $procmux$145 $procmux$146_CMP0 $procmux$143
	if (posedge_p_clk) {
		p_ctl__clr__sticky__pulse.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? (p_pi__d__in.slice<3>().val() ? value<1>{0x1u} : value<1>{0u}) : value<1>{0u}) : value<1>{0u});
	}
	// cells $procdff$757 $procmux$371 $procmux$337 $procmux$338_CMP0 $procmux$339_CMP0 $procmux$340_CMP0 $procmux$341_CMP0 $procmux$345_CMP0 $procmux$346_CMP0 $procmux$347_CMP0 $procmux$353_CMP0 $procmux$362_CMP0 $procmux$363_CMP0 $procmux$364_CMP0 $procmux$370_CMP0 $procmux$343 $procmux$351 $logic_or$../rtl/pistorm_psp2.v:441$66 $reduce_and$../rtl/pistorm_psp2.v:441$65 $procmux$348 $eq$../rtl/pistorm_psp2.v:443$67 $ternary$../rtl/pistorm_psp2.v:444$68 $procmux$360 $procmux$357 $procmux$354 $ternary$../rtl/pistorm_psp2.v:432$63 $procmux$368 $procmux$365 $ternary$../rtl/pistorm_psp2.v:412$56
	if (posedge_p_clk) {
		p_eng.next = (p_por ? value<4>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xbu}) ? value<4>{0x5u} : (eq_uu<1>(p_eng.curr, value<4>{0xcu}) ? value<4>{0x7u} : (eq_uu<1>(p_eng.curr, value<4>{0x8u}) ? value<4>{0x9u} : (eq_uu<1>(p_eng.curr, value<4>{0x7u}) ? value<4>{0x8u} : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? (p_attr__rd.curr ? value<4>{0x7u} : value<4>{0u}) : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<4>{0x6u} : (eq_uu<1>(p_eng.curr, value<4>{0x4u}) ? value<4>{0x5u} : (eq_uu<1>(p_eng.curr, value<4>{0xau}) ? (logic_or<1>(p_berr__a, reduce_and<1>(p_wd.curr)) ? value<4>{0xbu} : (eq_uu<1>(p_e__cnt.curr, value<4>{0x9u}) ? (p_attr__rd.curr ? value<4>{0x4u} : value<4>{0x5u}) : i_procmux_24_334__Y)) : (eq_uu<1>(p_eng.curr, value<4>{0x3u}) ? (i_logic__or_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_429_24_62__Y ? value<4>{0xbu} : (p_s__dtack.curr.slice<1>().val() ? (p_s__vpa.curr.slice<1>().val() ? i_procmux_24_334__Y : value<4>{0xau}) : (p_attr__rd.curr ? value<4>{0x4u} : value<4>{0x5u}))) : (eq_uu<1>(p_eng.curr, value<4>{0x2u}) ? value<4>{0x3u} : (eq_uu<1>(p_eng.curr, value<4>{0x1u}) ? value<4>{0x2u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<4>{0xcu} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? (p_attr__rd.curr ? value<4>{0x3u} : value<4>{0x2u}) : i_procmux_24_334__Y)) : value<4>{0u})))))))))))));
	}
	// cells $procdff$758 $procmux$328 $procmux$324 $procmux$325_CMP0 $procmux$326_CMP0 $procmux$327_CMP0 $add$../rtl/pistorm_psp2.v:440$64 $add$../rtl/pistorm_psp2.v:428$60
	if (posedge_p_clk) {
		p_wd.next = (p_por ? value<10>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xau}) ? add_uu<10>(p_wd.curr, value<10>{0x1u}) : (eq_uu<1>(p_eng.curr, value<4>{0x3u}) ? add_uu<10>(p_wd.curr, value<10>{0x1u}) : (logic_not<1>(p_eng.curr) ? value<10>{0u} : p_wd.curr))));
	}
	// cells $procdff$759 $procmux$313 $procmux$311 $procmux$312_CMP0 $procmux$309
	if (posedge_p_clk) {
		p_st__berr.next = (p_por ? value<1>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xbu}) ? (p_s__berr.curr.slice<1>().val() ? i_procmux_24_305__Y : value<1>{0x1u}) : i_procmux_24_305__Y));
	}
	// cells $procdff$760 $procmux$303 $procmux$301 $procmux$302_CMP0 $procmux$299
	if (posedge_p_clk) {
		p_st__wd.next = (p_por ? value<1>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xbu}) ? (p_s__berr.curr.slice<1>().val() ? value<1>{0x1u} : i_procmux_24_295__Y) : i_procmux_24_295__Y));
	}
	// cells $procdff$761 $procmux$293 $procmux$291 $procmux$289 $logic_or$../rtl/pistorm_psp2.v:295$33 $logic_not$../rtl/pistorm_psp2.v:295$32
	if (posedge_p_clk) {
		p_st__fight.next = (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : (p_wr__edge ? (logic_or<1>(logic_not<1>(p_ltch__d__rd__oe__n.curr), p_pi__d__oe.curr) ? value<1>{0x1u} : p_st__fight.curr) : p_st__fight.curr));
	}
	// cells $procdff$762 $procmux$287 $procmux$285 $procmux$286_CMP0
	if (posedge_p_clk) {
		p_dbg__mode.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? p_pi__d__in.slice<4>().val() : p_dbg__mode.curr) : p_dbg__mode.curr);
	}
	// cells $procdff$763 $procmux$283 $procmux$284_CMP0 $procmux$281 $procmux$278 $procmux$276 $add$../rtl/pistorm_psp2.v:403$52
	if (posedge_p_clk) {
		p_cnt__wr.next = (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? p_cnt__wr.curr : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? (p_attr__rd.curr ? p_cnt__wr.curr : add_uu<7>(p_cnt__wr.curr, value<7>{0x1u})) : p_cnt__wr.curr)) : p_cnt__wr.curr);
	}
	// cells $procdff$764 $procmux$260 $procmux$261_CMP0 $procmux$258 $procmux$255 $procmux$253 $add$../rtl/pistorm_psp2.v:402$51
	if (posedge_p_clk) {
		p_cnt__rd.next = (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? p_cnt__rd.curr : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp2_2e_v_3a_400_24_50__Y ? (p_attr__rd.curr ? add_uu<8>(p_cnt__rd.curr, value<8>{0x1u}) : p_cnt__rd.curr) : p_cnt__rd.curr)) : p_cnt__rd.curr);
	}
	// cells $procdff$765 $procmux$237 $procmux$235 $procmux$236_CMP0
	if (posedge_p_clk) {
		p_dbg__page2.next = (p_wr__edge ? (eq_uu<1>(p_pi__cmd, value<2>{0x3u}) ? p_pi__d__in.slice<5>().val() : p_dbg__page2.curr) : p_dbg__page2.curr);
	}
	// cells $procdff$766 $procmux$233 $logic_and$../rtl/pistorm_psp2.v:360$36 $logic_not$../rtl/pistorm_psp2.v:360$35 $add$../rtl/pistorm_psp2.v:360$37
	if (posedge_p_clk) {
		p_cnt__br.next = (logic_and<1>(p_br__a, logic_not<1>(p_br__prev.curr)) ? add_uu<8>(p_cnt__br.curr, value<8>{0x1u}) : p_cnt__br.curr);
	}
	// cells $procdff$767 $procmux$231 $logic_and$../rtl/pistorm_psp2.v:361$39 $logic_not$../rtl/pistorm_psp2.v:361$38 $add$../rtl/pistorm_psp2.v:361$40
	if (posedge_p_clk) {
		p_cnt__bgack.next = (logic_and<1>(p_bgack__a, logic_not<1>(p_bgack__prev.curr)) ? add_uu<8>(p_cnt__bgack.curr, value<8>{0x1u}) : p_cnt__bgack.curr);
	}
	// \src: ../rtl/pistorm_psp2.v:275.5-524.8
	// cell $procdff$768
	if (posedge_p_clk) {
		p_br__prev.next = p_br__a;
	}
	// \src: ../rtl/pistorm_psp2.v:275.5-524.8
	// cell $procdff$769
	if (posedge_p_clk) {
		p_bgack__prev.next = p_bgack__a;
	}
	// cells $procdff$770 $procmux$229 $add$../rtl/pistorm_psp2.v:506$78
	if (posedge_p_clk) {
		p_por__cnt.next = (p_por ? add_uu<3>(p_por__cnt.curr, value<3>{0x1u}) : p_por__cnt.curr);
	}
	// cells $procdff$771 $ge$../rtl/pistorm_psp2.v:214$19
	if (posedge_p_clk) {
		p_e__clk.next = ge_uu<1>(p_e__cnt.curr, value<4>{0x5u});
	}
	// cells $procdff$772 $ternary$../rtl/pistorm_psp2.v:213$18 $eq$../rtl/pistorm_psp2.v:213$16 $add$../rtl/pistorm_psp2.v:213$17
	if (posedge_p_clk) {
		p_e__cnt.next = (eq_uu<1>(p_e__cnt.curr, value<4>{0x9u}) ? value<4>{0u} : add_uu<4>(p_e__cnt.curr, value<4>{0x1u}));
	}
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$773
	if (posedge_p_clk) {
		p_s__wr.next = p_s__wr.curr.slice<0>().concat(p_pi__wr).val();
	}
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$774
	if (posedge_p_clk) {
		p_s__rd.next = p_s__rd.curr.slice<0>().concat(p_pi__rd).val();
	}
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$775
	if (posedge_p_clk) {
		p_s__dtack.next = p_s__dtack.curr.slice<0>().concat(p_dtack__n).val();
	}
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$776
	if (posedge_p_clk) {
		p_s__berr.next = p_s__berr.curr.slice<0>().concat(p_berr__n).val();
	}
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$777
	if (posedge_p_clk) {
		p_s__vpa.next = p_s__vpa.curr.slice<0>().concat(p_vpa__n).val();
	}
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$778
	if (posedge_p_clk) {
		p_s__br.next = p_s__br.curr.slice<0>().concat(p_br__n).val();
	}
	// \src: ../rtl/pistorm_psp2.v:183.5-194.8
	// cell $procdff$779
	if (posedge_p_clk) {
		p_s__bgack.next = p_s__bgack.curr.slice<0>().concat(p_bgack__n).val();
	}
	// connection
	p_rw = p_rw__i.curr;
	// connection
	p_vma__n = p_vma__i.curr;
	// connection
	p_bg__n = p_bg__i.curr;
	return converged;
}

void p_pistorm__psp2__core::debug_eval() {
	// cells $and$../rtl/pistorm_psp2.v:199$7 $not$../rtl/pistorm_psp2.v:199$6
	p_wr__edge = and_uu<1>(p_s__wr.curr.slice<1>().val(), not_u<1>(p_wr__prev.curr));
	// \src: ../rtl/pistorm_psp2.v:252.27-252.40
	// cell $eq$../rtl/pistorm_psp2.v:252$21
	p_engine__idle = logic_not<1>(p_eng.curr);
	// cells $and$../rtl/pistorm_psp2.v:200$9 $not$../rtl/pistorm_psp2.v:200$8
	p_rd__edge = and_uu<1>(p_s__rd.curr.slice<1>().val(), not_u<1>(p_rd__prev.curr));
	// \src: ../rtl/pistorm_psp2.v:203.20-203.30
	// cell $not$../rtl/pistorm_psp2.v:203$11
	p_berr__a = not_u<1>(p_s__berr.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp2.v:205.20-205.28
	// cell $not$../rtl/pistorm_psp2.v:205$13
	p_br__a = not_u<1>(p_s__br.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp2.v:223.22-223.34
	// cell $eq$../rtl/pistorm_psp2.v:223$20
	p_ext__owns = eq_uu<1>(p_arb.curr, value<2>{0x2u});
	// \src: ../rtl/pistorm_psp2.v:206.20-206.31
	// cell $not$../rtl/pistorm_psp2.v:206$14
	p_bgack__a = not_u<1>(p_s__bgack.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp2.v:273.22-273.37
	// cell $ne$../rtl/pistorm_psp2.v:273$22
	p_por = ne_uu<1>(p_por__cnt.curr, value<3>{0x7u});
	// connection
	p_wr__sync = p_s__wr.curr.slice<1>().val();
	// connection
	p_rd__sync = p_s__rd.curr.slice<1>().val();
}

CXXRTL_EXTREMELY_COLD
void p_pistorm__psp2__core::debug_info(debug_items *items, debug_scopes *scopes, std::string path, metadata_map &&cell_attrs) {
	assert(path.empty() || path[path.size() - 1] == ' ');
	if (scopes) {
		scopes->add(path.empty() ? path : path.substr(0, path.size() - 1), "pistorm_psp2_core", metadata_map({
			{ "top", UINT64_C(1) },
			{ "src", "../rtl/pistorm_psp2.v:17.1-526.10" },
		}), std::move(cell_attrs));
	}
	if (items) {
		items->add(path, "por", "src\000s../rtl/pistorm_psp2.v:273.15-273.18\000", debug_eval_outline, p_por);
		items->add(path, "por_cnt", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:272.15-272.22\000", p_por__cnt, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "bgack_prev", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:262.25-262.35\000", p_bgack__prev, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "br_prev", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:262.9-262.16\000", p_br__prev, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "cnt_bgack", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:261.30-261.39\000", p_cnt__bgack, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "cnt_br", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:261.15-261.21\000", p_cnt__br, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "dbg_page2", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:260.9-260.18\000", p_dbg__page2, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "cnt_rd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:259.15-259.21\000", p_cnt__rd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "cnt_wr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:258.15-258.21\000", p_cnt__wr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "dbg_mode", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:257.9-257.17\000", p_dbg__mode, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_fight", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:256.9-256.17\000", p_st__fight, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_wd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:255.25-255.30\000", p_st__wd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_berr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:255.9-255.16\000", p_st__berr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "wd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:254.16-254.18\000", p_wd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "eng", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:251.16-251.19\000", p_eng, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "ctl_clr_sticky_pulse", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:241.9-241.29\000", p_ctl__clr__sticky__pulse, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "gd_wr_sr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:238.16-238.24\000", p_gd__wr__sr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "ga_hi_sr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:237.16-237.24\000", p_ga__hi__sr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "ga_lo_sr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:236.16-236.24\000", p_ga__lo__sr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "go_arm", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:235.16-235.22\000", p_go__arm, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "csr_rd_pending", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:234.16-234.30\000", p_csr__rd__pending, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "attr_a0", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:233.16-233.23\000", p_attr__a0, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "attr_fc", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:232.16-232.23\000", p_attr__fc, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "attr_byte", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:231.32-231.41\000", p_attr__byte, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "attr_rd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:231.16-231.23\000", p_attr__rd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "go_pending", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:230.16-230.26\000", p_go__pending, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_bgack_seen", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:225.28-225.41\000", p_st__bgack__seen, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_br_seen", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:225.9-225.19\000", p_st__br__seen, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "ext_owns", "src\000s../rtl/pistorm_psp2.v:223.10-223.18\000", debug_eval_outline, p_ext__owns);
		items->add(path, "engine_idle", "src\000s../rtl/pistorm_psp2.v:222.10-222.21\000", debug_eval_outline, p_engine__idle);
		items->add(path, "arb", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:221.15-221.18\000", p_arb, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "e_cnt", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:211.15-211.20\000", p_e__cnt, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "bgack_a", "src\000s../rtl/pistorm_psp2.v:206.10-206.17\000", debug_eval_outline, p_bgack__a);
		items->add(path, "br_a", "src\000s../rtl/pistorm_psp2.v:205.10-205.14\000", debug_eval_outline, p_br__a);
		items->add(path, "berr_a", "src\000s../rtl/pistorm_psp2.v:203.10-203.16\000", debug_eval_outline, p_berr__a);
		items->add(path, "rd_edge", "src\000s../rtl/pistorm_psp2.v:200.10-200.17\000", debug_eval_outline, p_rd__edge);
		items->add(path, "wr_edge", "src\000s../rtl/pistorm_psp2.v:199.10-199.17\000", debug_eval_outline, p_wr__edge);
		items->add(path, "rd_prev", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:198.26-198.33\000", p_rd__prev, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "wr_prev", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:198.10-198.17\000", p_wr__prev, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "rd_sync", "src\000s../rtl/pistorm_psp2.v:197.10-197.17\000", debug_eval_outline, p_rd__sync);
		items->add(path, "wr_sync", "src\000s../rtl/pistorm_psp2.v:196.10-196.17\000", debug_eval_outline, p_wr__sync);
		items->add(path, "s_ipl2", "init\000u\000\000\000\000\000\000\000\007src\000s../rtl/pistorm_psp2.v:181.33-181.39\000", p_s__ipl2, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_ipl1", "init\000u\000\000\000\000\000\000\000\007src\000s../rtl/pistorm_psp2.v:181.15-181.21\000", p_s__ipl1, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_bgack", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp2.v:180.32-180.39\000", p_s__bgack, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_br", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp2.v:180.15-180.19\000", p_s__br, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_vpa", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp2.v:179.49-179.54\000", p_s__vpa, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_berr", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp2.v:179.32-179.38\000", p_s__berr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_dtack", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp2.v:179.15-179.22\000", p_s__dtack, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_rd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:178.32-178.36\000", p_s__rd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_wr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:178.15-178.19\000", p_s__wr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "lds_q", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:70.36-70.41\000", p_lds__q, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "uds_q", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:70.22-70.27\000", p_uds__q, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "as_q", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:70.9-70.13\000", p_as__q, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "bg_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:69.23-69.27\000", p_bg__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "vma_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:69.9-69.14\000", p_vma__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "rw_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:68.50-68.54\000", p_rw__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "lds_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:68.36-68.41\000", p_lds__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "uds_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:68.22-68.27\000", p_uds__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "as_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:68.9-68.13\000", p_as__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "bgack_n", "src\000s../rtl/pistorm_psp2.v:64.24-64.31\000", p_bgack__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "bg_n", "src\000s../rtl/pistorm_psp2.v:63.24-63.28\000", p_bg__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "br_n", "src\000s../rtl/pistorm_psp2.v:62.24-62.28\000", p_br__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "halt_drive", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:61.24-61.34\000", p_halt__drive, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "reset_drive", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:60.24-60.35\000", p_reset__drive, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "reset_n_in", "src\000s../rtl/pistorm_psp2.v:59.24-59.34\000", p_reset__n__in, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "ipl_n", "src\000s../rtl/pistorm_psp2.v:58.24-58.29\000", p_ipl__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "vma_n", "src\000s../rtl/pistorm_psp2.v:57.24-57.29\000", p_vma__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "e_clk", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:56.24-56.29\000", p_e__clk, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "vpa_n", "src\000s../rtl/pistorm_psp2.v:55.24-55.29\000", p_vpa__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "berr_n", "src\000s../rtl/pistorm_psp2.v:54.24-54.30\000", p_berr__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "dtack_n", "src\000s../rtl/pistorm_psp2.v:53.24-53.31\000", p_dtack__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "rw", "src\000s../rtl/pistorm_psp2.v:52.24-52.26\000", p_rw, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "lds_n", "src\000s../rtl/pistorm_psp2.v:51.24-51.29\000", p_lds__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "uds_n", "src\000s../rtl/pistorm_psp2.v:50.24-50.29\000", p_uds__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "as_n", "src\000s../rtl/pistorm_psp2.v:49.24-49.28\000", p_as__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "bus_oe", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:48.24-48.30\000", p_bus__oe, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "fc_out", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:47.24-47.30\000", p_fc__out, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_rd_oe_n", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:44.24-44.38\000", p_ltch__d__rd__oe__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_rd_l", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:43.24-43.35\000", p_ltch__d__rd__l, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_rd_u", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:42.24-42.35\000", p_ltch__d__rd__u, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_wr_oe_n", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:41.24-41.38\000", p_ltch__d__wr__oe__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_wr_l", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:40.24-40.35\000", p_ltch__d__wr__l, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_wr_u", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:39.24-39.35\000", p_ltch__d__wr__u, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_a_oe_n", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp2.v:38.24-38.35\000", p_ltch__a__oe__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_a_24", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:37.24-37.33\000", p_ltch__a__24, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_a_16", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:36.24-36.33\000", p_ltch__a__16, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_a_8", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:35.24-35.32\000", p_ltch__a__8, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_a_0", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:34.24-34.32\000", p_ltch__a__0, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_ipl2", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:31.24-31.31\000", p_pi__ipl2, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_ipl1", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:30.24-30.31\000", p_pi__ipl1, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_berr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:29.24-29.31\000", p_pi__berr, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "busy", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:27.24-27.28\000", p_busy, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_d_oe", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:26.24-26.31\000", p_pi__d__oe, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_d_out", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp2.v:25.24-25.32\000", p_pi__d__out, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_d_in", "src\000s../rtl/pistorm_psp2.v:24.24-24.31\000", p_pi__d__in, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "pi_cmd", "src\000s../rtl/pistorm_psp2.v:23.24-23.30\000", p_pi__cmd, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "pi_rd", "src\000s../rtl/pistorm_psp2.v:22.24-22.29\000", p_pi__rd, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "pi_wr", "src\000s../rtl/pistorm_psp2.v:21.24-21.29\000", p_pi__wr, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "clk", "src\000s../rtl/pistorm_psp2.v:18.24-18.27\000", p_clk, 0, debug_item::INPUT|debug_item::UNDRIVEN);
	}
}

} // namespace cxxrtl_design

extern "C"
cxxrtl_toplevel cxxrtl_design_create() {
	return new _cxxrtl_toplevel { std::unique_ptr<cxxrtl_design::p_pistorm__psp2__core>(new cxxrtl_design::p_pistorm__psp2__core) };
}
