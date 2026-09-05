// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <fstream>
#include "btop_log.hpp"

#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_cli.hpp"
#include "btop_shared.hpp"

using namespace std::string_literals;

namespace {
	Net::net_info traffic(uint64_t total, uint64_t speed = 0) {
		Net::net_info info;
		info.stat["download"].last = total;
		info.stat["download"].speed = speed;
		return info;
	}

	class InterfaceModel : public testing::Test {
	protected:
		void SetUp() override {
			Config::set("iface_include", ""s);
			Config::set("iface_exclude", ""s);
			Config::set("iface_view", "detail"s);
			Config::set("iface_sorting", "total"s);
			Config::set("iface_reversed", false);
			Config::set("net_iface", ""s);
			Config::set("net_auto", false);
			Config::set("net_download", 10);
			Config::set("net_upload", 10);
			Net::explicit_iface.reset();
			Net::explicit_unavailable = false;
			Net::selected_iface.clear();
			Net::confirmed_interfaces.clear();
			Net::iface_index = 0;
			Net::iface_page = 0;
			Net::set_page_size(1);
			Net::iface_compact_view_active = false;
			Net::compact_view_initialized = false;
			Net::interfaces.clear();
			Net::current_net.clear();
			Config::set("proc_filter", ""s);
			Net::clear_owner = Net::filter_target::proc;
			Net::normalize_filters();
		}
	};
}

TEST_F(InterfaceModel, FiltersWithSearchSemanticsAndExclusionWins) {
	Net::current_net = {{"eth2", traffic(2)}, {"eth10", traffic(1)}, {"wlan0", traffic(3)}};
	Net::interfaces = {"eth10", "wlan0", "eth2"};
	Config::set("iface_include", "eth"s);
	Config::set("iface_exclude", "10$"s);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"eth2"}));
}

TEST_F(InterfaceModel, NaturalNamesBreakMetricTies) {
	Net::current_net = {{"eth10", traffic(4)}, {"eth2", traffic(4)}, {"eth1", traffic(4)}};
	Net::interfaces = {"eth10", "eth2", "eth1"};
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"eth1", "eth2", "eth10"}));
	Config::set("iface_reversed", true);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"eth10", "eth2", "eth1"}));
}

TEST_F(InterfaceModel, ExplicitSelectionOverridesFiltersAndRecovers) {
	Net::explicit_iface = "wlan0";
	Config::set("iface_exclude", ".*"s);
	Net::current_net = {{"wlan0", traffic(1)}};
	Net::interfaces = {"wlan0"};
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "wlan0");
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"wlan0"}));

	Net::interfaces.clear();
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_TRUE(Net::explicit_unavailable);
	EXPECT_TRUE(Net::selected_iface.empty());
}

TEST(CompactInterfaceModel, CalculatesPagedGridGeometry) {
	const auto layout = Net::compact_layout(74, 11, 20, 7);
	EXPECT_EQ(layout.columns, 3);
	EXPECT_EQ(layout.rows, 3);
	EXPECT_EQ(layout.per_page, 9);
	EXPECT_EQ(layout.page, 0);
	EXPECT_EQ(layout.first, 0);
	EXPECT_EQ(layout.tile_width, 24);

	const auto narrow = Net::compact_layout(14, 5, 4, 3);
	EXPECT_EQ(narrow.columns, 1);
	EXPECT_EQ(narrow.rows, 1);
	EXPECT_EQ(narrow.per_page, 1);
	EXPECT_EQ(narrow.page, 3);
	EXPECT_GE(narrow.meter_width, 3);
}

TEST(CompactInterfaceModel, AutoScaleRaisesImmediatelyAndLowersAfterFiveSamples) {
	Net::compact_scale scale;
	EXPECT_EQ(scale.update(20 << 10, 0), 26U << 10);
	for (int sample = 0; sample < 4; sample++)
		EXPECT_EQ(scale.update(1 << 10, 0), 26U << 10);
	EXPECT_EQ(scale.update(1 << 10, 0), 10U << 10);
}

TEST(CompactInterfaceModel, AutoScaleResetsLowSamplesWhenPageChangesOrTrafficRises) {
	Net::compact_scale scale;
	scale.update(100 << 10, 0);
	for (int sample = 0; sample < 4; sample++) scale.update(1 << 10, 0);
	EXPECT_EQ(scale.update(1 << 10, 1), 130U << 10);
	for (int sample = 0; sample < 4; sample++) scale.update(1 << 10, 1);
	EXPECT_EQ(scale.update(20 << 10, 1), 130U << 10);
	EXPECT_EQ(scale.update(1 << 10, 1), 130U << 10);
}

TEST_F(InterfaceModel, InventoryIsUnmodifiedAndClearingRestoresAll) {
	Net::current_net = {{"eth2", traffic(2)}, {"veth10", traffic(3)}, {"br0", traffic(1)}};
	Net::interfaces = {"br0", "eth2", "veth10"};
	const auto inventory = Net::interfaces;
	Config::set("iface_include", "eth"s);
	Config::set("iface_exclude", "veth"s);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::interfaces, inventory);
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"eth2"}));
	Net::clear_filter(Net::filter_target::iface);
	EXPECT_EQ(Net::confirmed_interfaces.size(), 3U);
	EXPECT_EQ(Net::selected_iface, "eth2");
}

TEST_F(InterfaceModel, InvalidConfigPatternsNormalizeIndependently) {
	Config::set("iface_include", "["s);
	Config::set("iface_exclude", "veth"s);
	Net::normalize_filters();
	EXPECT_TRUE(Config::getS("iface_include").empty());
	EXPECT_EQ(Config::getS("iface_exclude"), "veth");
	Config::set("iface_include", "eth"s);
	Config::set("iface_exclude", "("s);
	Net::normalize_filters();
	EXPECT_EQ(Config::getS("iface_include"), "eth");
	EXPECT_TRUE(Config::getS("iface_exclude").empty());
}

TEST_F(InterfaceModel, ConfigSelectorRecoversAndDoesNotPinNavigation) {
	Config::set("net_iface", "eth2"s);
	Net::current_net = {{"eth2", traffic(1)}, {"eth10", traffic(3)}};
	Net::interfaces = {"eth2", "eth10"};
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth2");
	Net::selected_iface = "eth10";
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth10");
	Net::interfaces = {"eth10"};
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_TRUE(Net::explicit_unavailable);
	Net::interfaces.push_back("eth2");
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth2");
	EXPECT_FALSE(Net::explicit_unavailable);
}

TEST_F(InterfaceModel, CliSelectorOverridesConfigExactly) {
	Config::set("net_iface", "missing"s);
	Net::explicit_iface = "ETH0";
	Net::current_net = {{"eth0", traffic(1)}};
	Net::interfaces = {"eth0"};
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_TRUE(Net::explicit_unavailable);
	Net::explicit_iface = "eth0";
	Config::set("iface_include", "nomatch"s);
	Config::set("iface_exclude", ".*"s);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth0");
}

TEST_F(InterfaceModel, SpeedSortingAndRawTotalIgnoreDisplayOffset) {
	Net::current_net = {{"eth2", traffic(100, 1)}, {"eth10", traffic(20, 100)}};
	Net::interfaces = {"eth10", "eth2"};
	Net::current_net.at("eth2").stat.at("download").offset = 100;
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::confirmed_interfaces.front(), "eth2");
	Config::set("iface_sorting", "speed"s);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::confirmed_interfaces.front(), "eth10");
	EXPECT_EQ(Net::selected_iface, "eth2");
}

TEST_F(InterfaceModel, DeleteOwnershipFallsBackToNonemptyTarget) {
	Config::set("proc_filter", "btop"s);
	Config::set("iface_include", "eth"s);
	Net::clear_owner = Net::filter_target::iface;
	EXPECT_EQ(Net::delete_target(), Net::filter_target::iface);
	Net::clear_owner = Net::filter_target::proc;
	EXPECT_EQ(Net::delete_target(), Net::filter_target::proc);
	Net::clear_filter(Net::delete_target());
	EXPECT_EQ(Net::delete_target(), Net::filter_target::iface);
	Net::clear_filter(Net::delete_target());
	EXPECT_EQ(Net::delete_target(), Net::filter_target::none);
}

TEST_F(InterfaceModel, EditorIsAtomicAndReportsBothInvalidFields) {
	Config::set("iface_include", "eth"s);
	Config::set("iface_exclude", "10"s);
	Net::interface_editor editor;
	editor.open();
	EXPECT_EQ(editor.drafts[0].text, "eth");
	EXPECT_EQ(editor.drafts[1].text, "10");
	editor.drafts = {Draw::TextEdit{"["}, Draw::TextEdit{"("}};
	EXPECT_FALSE(editor.command("enter"));
	EXPECT_TRUE(editor.active);
	EXPECT_TRUE(editor.invalid[0]);
	EXPECT_TRUE(editor.invalid[1]);
	EXPECT_EQ(Config::getS("iface_include"), "eth");
	EXPECT_EQ(Config::getS("iface_exclude"), "10");
	editor.drafts[0] = Draw::TextEdit{"wlan"};
	EXPECT_FALSE(editor.command("enter"));
	EXPECT_FALSE(editor.invalid[0]);
	EXPECT_TRUE(editor.invalid[1]);
	EXPECT_EQ(Config::getS("iface_include"), "eth");
	editor.command("escape");
	EXPECT_FALSE(editor.active);
	EXPECT_EQ(Net::clear_owner, Net::filter_target::proc);
}

TEST_F(InterfaceModel, EditorSwitchesEditsIgnoresDeleteAndConfirmsRuntimeView) {
	Net::interface_editor editor;
	editor.open();
	editor.command("e"); editor.command("t"); editor.command("h");
	editor.command("left"); editor.command("backspace"); editor.command("t");
	editor.command("delete");
	EXPECT_EQ(editor.drafts[0].text, "eth");
	editor.command("tab");
	EXPECT_EQ(editor.field, 1);
	editor.command("x");
	editor.command("shift_tab");
	EXPECT_EQ(editor.field, 0);
	EXPECT_TRUE(Config::getS("iface_include").empty());
	Config::set("iface_view", "detail"s);
	EXPECT_TRUE(editor.command("enter"));
	EXPECT_EQ(Config::getS("iface_include"), "eth");
	EXPECT_EQ(Config::getS("iface_exclude"), "x");
	EXPECT_EQ(Config::getS("iface_view"), "detail");
	EXPECT_TRUE(Net::iface_compact_view_active);
	EXPECT_EQ(Net::clear_owner, Net::filter_target::iface);
	editor.open(); editor.command("x"); editor.command("mouse_click");
	EXPECT_EQ(Config::getS("iface_include"), "eth");
}

TEST(InterfaceCli, RepeatedSelectorUsesLastValue) {
	const std::array<std::string_view, 4> args{"--iface", "eth2", "--iface", "eth10"};
	const auto result = Cli::parse(args);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->iface, "eth10");
}

TEST_F(InterfaceModel, PatternMatrixUsesCaseSensitivePosixSearch) {
	Net::current_net = {{"eth0", traffic(1)}, {"wlan0", traffic(1)}, {"veth_mira2", traffic(1)}, {"br0", traffic(1)}, {"tun0", traffic(1)}};
	Net::interfaces = {"eth0", "wlan0", "veth_mira2", "br0", "tun0"};
	struct Case { string include, exclude; std::vector<string> expected; };
	const std::vector<Case> cases{
		{"", "", {"br0", "eth0", "tun0", "veth_mira2", "wlan0"}},
		{"eth", "", {"eth0", "veth_mira2"}},
		{"", "eth", {"br0", "tun0", "wlan0"}},
		{"eth", "^veth", {"eth0"}},
		{"eth", "eth", {}},
		{"ETH", "", {}},
		{"^(br|tun)[[:digit:]]+$", "", {"br0", "tun0"}},
	};
	for (const auto& item : cases) {
		SCOPED_TRACE(item.include + "/" + item.exclude);
		Config::set("iface_include", item.include);
		Config::set("iface_exclude", item.exclude);
		Net::rebuild_interfaces(Net::current_net);
		EXPECT_EQ(Net::confirmed_interfaces, item.expected);
	}
}

TEST_F(InterfaceModel, RemovingUnavailableSelectorOnReloadRestoresSelection) {
	Net::interfaces = {"eth0"};
	Net::current_net = {{"eth0", traffic(1)}};
	Config::set("net_iface", "gone"s);
	Net::rebuild_interfaces(Net::current_net);
	ASSERT_TRUE(Net::explicit_unavailable);
	Config::set("net_iface", ""s);
	Net::normalize_filters();
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_FALSE(Net::explicit_unavailable);
	EXPECT_EQ(Net::selected_iface, "eth0");
}

TEST_F(InterfaceModel, NavigationWrapsAndReorderingMaintainsIndex) {
	Net::interfaces = {"eth10", "eth2", "eth1"};
	Net::current_net = {{"eth10", traffic(1)}, {"eth2", traffic(1)}, {"eth1", traffic(1)}};
	Net::rebuild_interfaces(Net::current_net);
	Net::navigate_interface(-1);
	EXPECT_EQ(Net::selected_iface, "eth10");
	EXPECT_EQ(Net::iface_index, 2);
	Config::set("iface_reversed", true);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth10");
	EXPECT_EQ(Net::iface_index, 0);
	Net::navigate_interface(1);
	EXPECT_EQ(Net::selected_iface, "eth2");
	Config::set("iface_exclude", ".*"s);
	Net::rebuild_interfaces(Net::current_net);
	Net::navigate_interface(1);
	EXPECT_TRUE(Net::selected_iface.empty());
	EXPECT_EQ(Net::iface_index, 0);
}

TEST_F(InterfaceModel, NaturalOrderingHandlesLeadingZerosAndLargeNumbers) {
	Net::interfaces = {"eth1x", "eth01", "eth1", "eth999999999999999999999999999999", "eth2"};
	for (const auto& name : Net::interfaces) Net::current_net.emplace(name, traffic(1));
	Config::set("iface_sorting", "alnum"s);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"eth01", "eth1", "eth1x", "eth2", "eth999999999999999999999999999999"}));
}

TEST(CompactInterfaceModel, RisingTrafficBelowThresholdRestartsCountdown) {
	Net::compact_scale scale;
	scale.update(100000, 0);
	for (int i = 0; i < 4; i++) scale.update(100, 0);
	EXPECT_EQ(scale.update(200, 0), 130000U);
	for (int i = 0; i < 4; i++) EXPECT_EQ(scale.update(200, 0), 130000U);
	EXPECT_EQ(scale.update(200, 0), 10240U);
}

TEST(CompactInterfaceModel, GridClampsAfterChurnAndMetersFitNarrowTiles) {
	for (int width = 14; width < 170; width++) {
		const auto layout = Net::compact_layout(width, 8, 50, 49);
		EXPECT_LE(layout.columns * layout.tile_width, width - 2);
		if (layout.tile_width < 3)
			EXPECT_EQ(layout.meter_width, 0);
		else
			EXPECT_LE(layout.meter_width + 2 + (layout.tile_width >= 14 ? 8 : 0), layout.tile_width);
		EXPECT_LE(layout.rows * 3, 6);
		EXPECT_LE(layout.first, 49);
		EXPECT_GT(layout.first + layout.per_page, 49);
		EXPECT_EQ(Net::compact_layout(width, 8, 0, 49).page, 0);
	}
}

TEST_F(InterfaceModel, InvalidConfigDirectiveIsLogged) {
	char filename[] = "/tmp/btop-interface-log-XXXXXX";
	const int fd = mkstemp(filename);
	ASSERT_GE(fd, 0);
	close(fd);
	Logger::init(filename);
	Logger::set_log_level(Logger::Level::WARNING);
	Config::set("iface_include", "["s);
	Config::set("iface_exclude", "eth"s);
	Net::normalize_filters();
	std::ifstream input(filename);
	const string log{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	EXPECT_NE(log.find("Invalid iface_include"), string::npos);
	EXPECT_EQ(log.find("Invalid iface_exclude"), string::npos);
	std::remove(filename);
}

TEST(InterfaceCli, SelectorRequiresAnArgument) {
	const std::array<std::string_view, 1> args{"--iface"};
	const auto result = Cli::parse(args);
	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error(), 0);
}

TEST(InterfaceEditorLayout, FitsMinimumAndWideTerminal) {
	for (const auto& dimensions : {std::pair{36, 6}, {80, 24}, {160, 40}}) {
		const auto layout = Net::filter_editor_layout(dimensions.first, dimensions.second);
		EXPECT_LE(layout.left + layout.width - 1, dimensions.first);
		EXPECT_LE(layout.top + layout.height - 1, dimensions.second);
		EXPECT_LT(layout.include_row, layout.exclude_row);
		EXPECT_LT(layout.exclude_row, layout.message_row);
		EXPECT_LT(layout.message_row, layout.top + layout.height - 1);
	}
}

TEST_F(InterfaceModel, ZeroOnlyNumericSegmentsCompareSuffixesNaturally) {
	Net::interfaces = {"eth00z", "eth0a", "eth0", "eth000"};
	for (const auto& name : Net::interfaces) Net::current_net.emplace(name, traffic(1));
	Config::set("iface_sorting", "alnum"s);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"eth0", "eth000", "eth0a", "eth00z"}));
}


TEST_F(InterfaceModel, SelectionRecoveryPolicyIsExplicitAndReusable) {
	const std::vector<string> confirmed{"eth0", "wlan0"};

	auto state = Net::reconcile_selection(confirmed, "wlan0", "eth0", true, false);
	EXPECT_EQ(state.selected, "eth0");
	EXPECT_EQ(state.index, 0);
	EXPECT_TRUE(state.explicit_seen);
	EXPECT_FALSE(state.explicit_unavailable);

	state = Net::reconcile_selection(confirmed, "missing", "eth0", true, false);
	EXPECT_TRUE(state.selected.empty());
	EXPECT_EQ(state.index, 0);
	EXPECT_FALSE(state.explicit_seen);
	EXPECT_TRUE(state.explicit_unavailable);

	state = Net::reconcile_selection(confirmed, "wlan0", "", false, true);
	EXPECT_EQ(state.selected, "wlan0");
	EXPECT_EQ(state.index, 1);
	EXPECT_TRUE(state.explicit_seen);
	EXPECT_FALSE(state.explicit_unavailable);

	state = Net::reconcile_selection(confirmed, "", "wlan0", false, true);
	EXPECT_EQ(state.selected, "wlan0");
	EXPECT_EQ(state.index, 1);
	EXPECT_FALSE(state.explicit_seen);
	EXPECT_FALSE(state.explicit_unavailable);
}

TEST_F(InterfaceModel, ExplicitSelectorOverridesEveryFilterCombinationExactly) {
	Net::interfaces = {"eth0", "wlan0"};
	Net::current_net = {{"eth0", traffic(1)}, {"wlan0", traffic(2)}};
	Net::explicit_iface = "eth0";
	for (const auto& item : std::vector<std::tuple<string, string, std::vector<string>>>{
		{"nomatch", "", {"eth0"}},
		{"eth", "eth", {"eth0"}},
		{"", "eth", {"wlan0", "eth0"}},
		{"nomatch", "eth", {"eth0"}}}) {
		SCOPED_TRACE(std::get<0>(item) + "/" + std::get<1>(item));
		const auto& include = std::get<0>(item);
		const auto& exclude = std::get<1>(item);
		const auto& interfaces = std::get<2>(item);
		Config::set("iface_include", include);
		Config::set("iface_exclude", exclude);
		Net::rebuild_interfaces(Net::current_net);
		EXPECT_EQ(Net::confirmed_interfaces, interfaces);
		EXPECT_EQ(Net::selected_iface, "eth0");
		EXPECT_FALSE(Net::explicit_unavailable);
	}
	EXPECT_FALSE(Net::has_interface("ETH0"));
	EXPECT_TRUE(Net::has_interface("eth0"));
}

TEST_F(InterfaceModel, ConfirmationPreservesSelectionAndEntersRuntimeCompactView) {
	Net::interfaces = {"eth0", "eth1", "wlan0"};
	for (const auto& name : Net::interfaces) Net::current_net.emplace(name, traffic(1));
	Net::rebuild_interfaces(Net::current_net);
	Net::selected_iface = "eth1";
	Net::iface_index = 1;
	Net::filter_editor.open();
	Net::filter_editor.drafts = {Draw::TextEdit{"eth|wlan"}, Draw::TextEdit{"wlan"}};
	EXPECT_TRUE(Net::filter_editor.command("enter"));
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"eth0", "eth1"}));
	EXPECT_EQ(Net::selected_iface, "eth1");
	EXPECT_EQ(Net::iface_index, 1);
	EXPECT_TRUE(Net::iface_compact_view_active);
	EXPECT_EQ(Config::getS("iface_view"), "detail");
	EXPECT_EQ(Net::clear_owner, Net::filter_target::iface);

	Net::filter_editor.open();
	Net::filter_editor.drafts = {Draw::TextEdit{"wlan"}, Draw::TextEdit{""}};
	EXPECT_TRUE(Net::filter_editor.command("enter"));
	EXPECT_EQ(Net::confirmed_interfaces, (std::vector<string>{"wlan0"}));
	EXPECT_EQ(Net::selected_iface, "wlan0");
	Net::iface_compact_view_active = false;
	EXPECT_EQ(Config::getS("iface_view"), "detail");
}

TEST_F(InterfaceModel, InventoryChurnAndFilterChangesPreserveSelection) {
	Net::interfaces = {"eth0", "eth1"};
	Net::current_net = {{"eth0", traffic(2)}, {"eth1", traffic(1)}};
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth0");
	Config::set("iface_include", "eth"s);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth0");
	Config::set("iface_exclude", "eth0"s);
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth1");
	Net::interfaces = {"eth1"};
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth1");
	Net::interfaces = {"eth0", "eth1"};
	Net::rebuild_interfaces(Net::current_net);
	EXPECT_EQ(Net::selected_iface, "eth1");
}

TEST_F(InterfaceModel, PagingClampsAndUsesOnlyVisibleTraffic) {
	Net::interfaces.clear();
	std::vector<string> names;
	std::unordered_map<string, Net::net_info> net;
	for (int i = 0; i < 12; i++) {
		names.push_back("eth" + std::to_string(i));
		auto& stat = net[names.back()].stat.at("download");
		stat.speed = i == 3 ? 300 : i == 8 ? 800 : 10;
	}
	EXPECT_EQ(Net::compact_page(3, 12, 4), 0);
	EXPECT_EQ(Net::compact_page(4, 12, 4), 1);
	EXPECT_EQ(Net::compact_page(99, 12, 4), 2);
	EXPECT_EQ(Net::compact_page(99, 0, 4), 0);
	EXPECT_EQ(Net::compact_page_sample(names, net, 0, 4), 300U);
	EXPECT_EQ(Net::compact_page_sample(names, net, 4, 4), 10U);
	EXPECT_EQ(Net::compact_page_sample(names, net, 8, 4), 800U);
	EXPECT_EQ(Net::compact_page_sample(names, net, 10, 4), 10U);
}

TEST(CompactInterfaceModel, NarrowTilesDegradeBeforeMetersShrink) {
	Net::net_info info;
	info.stat.at("download").speed = 25;
	info.stat.at("upload").speed = 50;

	const auto tiny = Net::compact_tile("eth0", info, 1, 100, 100);
	EXPECT_TRUE(tiny.name.empty());
	EXPECT_FALSE(tiny.show_speeds);
	const auto narrow = Net::compact_tile("eth0", info, 3, 100, 100);
	EXPECT_EQ(narrow.name, "e");
	EXPECT_FALSE(narrow.show_speeds);
	const auto wide = Net::compact_tile("eth0", info, 24, 100, 100);
	EXPECT_EQ(wide.name, "eth0");
	EXPECT_TRUE(wide.show_speeds);
	EXPECT_EQ(wide.download_percent, 25);
	EXPECT_EQ(wide.upload_percent, 50);
}

TEST(CompactInterfaceModel, GeometryFitsEveryWidth) {
	for (int width = 1; width < 170; width++) {
		const auto layout = Net::compact_layout(width, 8, 50, 49);
		EXPECT_LE(layout.columns * layout.tile_width, std::max(1, width - 2));
		EXPECT_LE(layout.rows * 3, 6);
		if (layout.tile_width < 3)
			EXPECT_EQ(layout.meter_width, 0);
		else if (layout.tile_width >= 14)
			EXPECT_LE(layout.meter_width + 10, layout.tile_width);
		else
			EXPECT_LE(layout.meter_width + 2, layout.tile_width);
	}
}

TEST(CompactInterfaceModel, FixedCeilingsAreIndependent) {
	Config::set("net_download", 8);
	Config::set("net_upload", 16);
	const auto limits = Net::fixed_net_limits();
	EXPECT_EQ(limits.download, 1U << 20);
	EXPECT_EQ(limits.upload, 2U << 20);
}

TEST_F(InterfaceModel, DeleteOwnershipAndAtomicClearingPreserveOtherFilter) {
	Config::set("proc_filter", "btop"s);
	Config::set("iface_include", "eth"s);
	Config::set("iface_exclude", "wlan"s);
	Net::clear_owner = Net::filter_target::iface;
	EXPECT_EQ(Net::delete_target(), Net::filter_target::iface);
	Net::clear_filter(Net::filter_target::iface);
	EXPECT_TRUE(Config::getS("iface_include").empty());
	EXPECT_TRUE(Config::getS("iface_exclude").empty());
	EXPECT_EQ(Config::getS("proc_filter"), "btop");
	EXPECT_EQ(Net::delete_target(), Net::filter_target::proc);
	Net::clear_filter(Net::filter_target::proc);
	EXPECT_TRUE(Config::getS("proc_filter").empty());
	EXPECT_EQ(Net::delete_target(), Net::filter_target::none);
	Net::clear_filter(Net::filter_target::none);
	EXPECT_EQ(Net::delete_target(), Net::filter_target::none);
}

TEST(ConfigDocumentation, DocumentsPatternsAndOmitsRuntimeState) {
	const auto config = Config::current_config();
	EXPECT_NE(config.find("Case-sensitive POSIX extended regular expression"), string::npos);
	EXPECT_NE(config.find("Empty includes all"), string::npos);
	EXPECT_NE(config.find("Empty excludes none"), string::npos);
	for (const auto& name : {"iface_compact_view_active", "confirmed_interfaces", "iface_index", "iface_page", "clear_owner"}) {
		EXPECT_EQ(config.find(string{name} + " ="), string::npos) << name;
	}
}
