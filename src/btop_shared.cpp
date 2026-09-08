/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include <sys/resource.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <regex>
#include <string>
#include <unordered_set>

#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_log.hpp"
#include "btop_shared.hpp"
#include "btop_tools.hpp"

namespace fs = std::filesystem;
namespace rng = std::ranges;
using namespace Tools;

namespace Net {
	std::optional<string> explicit_iface;
	bool explicit_unavailable{};
	vector<string> confirmed_interfaces;
	int iface_index{}, iface_page{};
	static int iface_page_size = 1;
	bool iface_compact_view_active{};
	bool compact_view_initialized{};
	static std::optional<std::regex> include_re;
	static std::optional<std::regex> exclude_re;
	static string compiled_include;
	static string compiled_exclude;
	static bool explicit_seen{};
	static string recovery_name;

	filter_target clear_owner = filter_target::proc;
	interface_editor filter_editor;

	filter_target delete_target() {
		const bool proc = not Config::getS("proc_filter").empty();
		const auto include = Config::getS("iface_include");
		const bool iface = (not include.empty() and include != ".*"s) or not Config::getS("iface_exclude").empty();
		if (proc and iface) return clear_owner;
		return proc ? filter_target::proc : iface ? filter_target::iface : filter_target::none;
	}

	void clear_filter(filter_target target) {
		if (target == filter_target::proc) Config::set("proc_filter", ""s);
		else if (target == filter_target::iface) {
			Config::set("iface_include", ".*"s);
			Config::set("iface_exclude", ""s);
			rebuild_interfaces(current_net);
		}
	}

	void interface_editor::open() {
		drafts = {Draw::TextEdit{Config::getS("iface_include")}, Draw::TextEdit{Config::getS("iface_exclude")}};
		invalid = {};
		field = 0;
		active = true;
	}

	bool interface_editor::command(std::string_view key) {
		if (key == "escape" or key == "mouse_click") active = false;
		else if (key == "tab" or key == "shift_tab") field = 1 - field;
		else if (key == "enter") {
			invalid = {};
			for (int i = 0; i < 2; i++) {
				try { if (not drafts[i].text.empty()) std::regex pattern(drafts[i].text, std::regex::extended); }
				catch (const std::regex_error&) { invalid[i] = true; }
			}
			if (invalid[0] or invalid[1]) return false;
			Config::set("iface_include", drafts[0].text);
			Config::set("iface_exclude", drafts[1].text);
			explicit_seen = false;
			rebuild_interfaces(current_net);
			iface_compact_view_active = true;
			compact_view_initialized = true;
			clear_owner = filter_target::iface;
			active = false;
			return true;
		}
		else if (key != "delete") drafts[field].command(key);
		return false;
	}

	static bool natural_less(const string& left, const string& right) {
		size_t left_pos = 0, right_pos = 0;
		// Compare digit spans without integer conversion, allowing arbitrarily long names.
		while (left_pos < left.size() and right_pos < right.size()) {
			if (std::isdigit(static_cast<unsigned char>(left[left_pos])) and std::isdigit(static_cast<unsigned char>(right[right_pos]))) {
				while (left_pos < left.size() and left[left_pos] == '0') left_pos++;
				while (right_pos < right.size() and right[right_pos] == '0') right_pos++;
				auto left_end = left_pos, right_end = right_pos;
				while (left_end < left.size() and std::isdigit(static_cast<unsigned char>(left[left_end]))) left_end++;
				while (right_end < right.size() and std::isdigit(static_cast<unsigned char>(right[right_end]))) right_end++;
				if (left_end - left_pos != right_end - right_pos) return left_end - left_pos < right_end - right_pos;
				if (auto cmp = left.substr(left_pos, left_end-left_pos).compare(right.substr(right_pos, right_end-right_pos)); cmp != 0) return cmp < 0;
				left_pos = left_end;
				right_pos = right_end;
				continue;
			}
			if (left[left_pos] != right[right_pos]) return left[left_pos] < right[right_pos];
			left_pos++; right_pos++;
		}
		if (left_pos != left.size() or right_pos != right.size()) return left_pos == left.size();
		return left < right;
	}

	static auto compile_pattern(const string& name, string& value) -> std::optional<std::regex> {
		if (value.empty()) return std::nullopt;
		try { return std::regex(value, std::regex::extended); }
		catch (const std::regex_error& e) {
			Logger::warning("Invalid {} pattern '{}': {}; disabling it", name, value, e.what());
			Config::set(name, ""s);
			value.clear();
			return std::nullopt;
		}
	}

	void normalize_filters() {
		auto include = Config::getS("iface_include");
		auto exclude = Config::getS("iface_exclude");
		include_re = compile_pattern("iface_include", include);
		exclude_re = compile_pattern("iface_exclude", exclude);
		compiled_include = include;
		compiled_exclude = exclude;
	}

	bool has_interface(const string& name) { return v_contains(interfaces, name); }

	uint64_t interface_total(const net_info& info) {
		return info.stat.at("download").last + info.stat.at("download").rollover
			+ info.stat.at("upload").last + info.stat.at("upload").rollover;
	}

	uint64_t interface_speed(const net_info& info) {
		return info.stat.at("download").speed + info.stat.at("upload").speed;
	}

	auto fixed_net_limits() -> compact_limits {
		const auto download = (static_cast<uint64_t>(Config::getI("net_download")) << 20) / 8;
		const auto upload = (static_cast<uint64_t>(Config::getI("net_upload")) << 20) / 8;
		return {download, upload};
	}

	auto reconcile_selection(const vector<string>& confirmed, const string& preferred, const string& selected,
		bool explicit_seen, bool explicit_unavailable) -> selection_state {
		selection_state state{selected, 0, explicit_seen, explicit_unavailable};
		const bool preferred_present = not preferred.empty() and v_contains(confirmed, preferred);
		if (preferred_present) {
			if (not state.explicit_seen or state.selected.empty()) state.selected = preferred;
			state.explicit_seen = true;
			state.explicit_unavailable = false;
		}
		else {
			state.explicit_seen = false;
			state.explicit_unavailable = not preferred.empty();
			if (not preferred.empty()) state.selected.clear();
		}
		if (not state.selected.empty() and not v_contains(confirmed, state.selected)) state.selected.clear();
		if (state.selected.empty() and not state.explicit_unavailable and not confirmed.empty())
			state.selected = confirmed.front();
		state.index = state.selected.empty() ? 0 : v_index(confirmed, state.selected);
		return state;
	}

	int compact_page(int index, int count, int per_page) {
		if (count <= 0 or per_page <= 0) return 0;
		return std::clamp(index, 0, count - 1) / per_page;
	}

	uint64_t compact_page_sample(const vector<string>& confirmed, const std::unordered_map<string, net_info>& net,
		int first, int count) {
		uint64_t sample{};
		for (int slot = 0; slot < count; slot++) {
			const int index = first + slot;
			if (index < 0 or index >= (int)confirmed.size()) break;
			const auto it = net.find(confirmed.at(index));
			if (it == net.end()) continue;
			sample = std::max(sample, std::max(it->second.stat.at("download").speed,
				it->second.stat.at("upload").speed));
		}
		return sample;
	}

	auto compact_tile(const string& name, const net_info& info, int tile_width,
		uint64_t download_limit, uint64_t upload_limit) -> compact_tile_info {
		const auto percent = [](uint64_t speed, uint64_t limit) {
			return static_cast<int>(std::min(100.0L, 100.0L * speed / std::max<uint64_t>(1, limit)));
		};
		return {
			uresize(name, std::max(0, tile_width - 2)),
			info.stat.at("download").speed,
			info.stat.at("upload").speed,
			percent(info.stat.at("download").speed, download_limit),
			percent(info.stat.at("upload").speed, upload_limit),
			tile_width >= 14
		};
	}

	auto compact_layout(int width, int height, int count, int selected) -> compact_layout_info {
		compact_layout_info layout;
		const int available_width = std::max(1, width - 2);
		const int available_height = std::max(1, height - 2);
		layout.columns = std::max(1, available_width / 24);
		layout.rows = std::max(1, available_height / 3);
		layout.per_page = layout.columns * layout.rows;
		layout.page = compact_page(selected, count, layout.per_page);
		layout.first = layout.page * layout.per_page;
		layout.tile_width = std::max(1, available_width / layout.columns);
		layout.meter_width = layout.tile_width < 3 ? 0
			: std::max(1, layout.tile_width - (layout.tile_width >= 14 ? 10 : 2));
		return layout;
	}

	bool compact_scale::change_page(int current_page) {
		if (page == current_page) return false;
		page = current_page;
		low_samples = 0;
		return true;
	}

	auto compact_scale::update(uint64_t sample, int current_page) -> uint64_t {
		const bool page_changed = change_page(current_page);
		const bool traffic_rose = sample > previous_sample;
		previous_sample = sample;
		if (sample >= ceiling) {
			ceiling = std::max<uint64_t>(10 << 10, sample * 13 / 10);
			low_samples = 0;
		}
		else if (sample < ceiling / 10 and not page_changed and not traffic_rose) {
			if (++low_samples >= 5) {
				ceiling = std::max<uint64_t>(10 << 10, sample * 3);
				low_samples = 0;
			}
		}
		else low_samples = 0;
		return ceiling;
	}

	void rebuild_interfaces(std::unordered_map<string, net_info>& net) {
		const auto& inventory = interfaces;
		if (compiled_include != Config::getS("iface_include") or compiled_exclude != Config::getS("iface_exclude")) normalize_filters();
		confirmed_interfaces.clear();
		for (const auto& name : inventory) {
			const bool included = not include_re or std::regex_search(name, *include_re);
			const bool excluded = exclude_re and std::regex_search(name, *exclude_re);
			if (included and not excluded) confirmed_interfaces.push_back(name);
		}
		const auto preferred = explicit_iface.value_or(Config::getS("net_iface"));
		if (preferred != recovery_name) {
			recovery_name = preferred;
			explicit_seen = false;
			explicit_unavailable = false;
		}
		const bool preferred_present = not preferred.empty() and v_contains(inventory, preferred);
		if (preferred_present and not v_contains(confirmed_interfaces, preferred)) confirmed_interfaces.push_back(preferred);

		const auto sorting = Config::getS("iface_sorting");
		const auto metric = [&](const string& name) {
			const auto it = net.find(name);
			if (it == net.end()) return uint64_t{};
			return sorting == "speed" ? interface_speed(it->second) : interface_total(it->second);
		};
		rng::sort(confirmed_interfaces, [&](const auto& a, const auto& b) {
			if (sorting != "alnum") {
				const auto av = metric(a);
				const auto bv = metric(b);
				if (av != bv) return av > bv;
			}
			return natural_less(a, b);
		});
		if (Config::getB("iface_reversed")) rng::reverse(confirmed_interfaces);

		const auto state = reconcile_selection(confirmed_interfaces, preferred, selected_iface, explicit_seen, explicit_unavailable);
		selected_iface = state.selected;
		explicit_seen = state.explicit_seen;
		explicit_unavailable = state.explicit_unavailable;
		iface_index = state.index;
		iface_page = compact_page(iface_index, (int)confirmed_interfaces.size(), iface_page_size);
	}

	void set_page_size(int per_page) {
		iface_page_size = std::max(1, per_page);
		iface_page = compact_page(iface_index, (int)confirmed_interfaces.size(), iface_page_size);
	}

	void navigate_interface(int direction) {
		if (selected_iface.empty() or confirmed_interfaces.empty()) return;
		iface_index = (iface_index + direction + confirmed_interfaces.size()) % confirmed_interfaces.size();
		selected_iface = confirmed_interfaces.at(iface_index);
		iface_page = compact_page(iface_index, (int)confirmed_interfaces.size(), iface_page_size);
		rescale = true;
	}
}

namespace Cpu {
    std::optional<std::string> container_engine;

	string trim_name(string name) {
		auto name_vec = ssplit(name);

		if ((name.contains("Xeon") or v_contains(name_vec, "Duo"s)) and v_contains(name_vec, "CPU"s)) {
			auto cpu_pos = v_index(name_vec, "CPU"s);
			if (cpu_pos < name_vec.size() - 1 and not name_vec.at(cpu_pos + 1).ends_with(')'))
				name = name_vec.at(cpu_pos + 1);
			else
				name.clear();
		} else if (v_contains(name_vec, "Ryzen"s)) {
			auto ryz_pos = v_index(name_vec, "Ryzen"s);
			name = "Ryzen";
			int tokens = 0;
			for (auto i = ryz_pos + 1; i < name_vec.size() && tokens < 2; i++) {
				const std::string& p = name_vec.at(i);
				if (p != "AI" && p != "PRO" && p != "H" && p != "HX")
					tokens++;
				name += " " + p;
			}
		} else if (name.contains("Intel") and v_contains(name_vec, "CPU"s)) {
			auto cpu_pos = v_index(name_vec, "CPU"s);
			if (cpu_pos < name_vec.size() - 1 and not name_vec.at(cpu_pos + 1).ends_with(')') and name_vec.at(cpu_pos + 1) != "@")
				name = name_vec.at(cpu_pos + 1);
			else
				name.clear();
		} else
			name.clear();

		if (name.empty() and not name_vec.empty()) {
			for (const auto &n : name_vec) {
				if (n == "@") break;
				name += n + ' ';
			}
			name.pop_back();
			for (const auto& replace : {"Processor", "CPU", "(R)", "(TM)", "Intel", "AMD", "Apple", "Core"}) {
				name = s_replace(name, replace, "");
				name = s_replace(name, "  ", " ");
			}
			name = trim(name);
		}

		return name;
	}
}

#ifdef GPU_SUPPORT
namespace Gpu {
	vector<string> gpu_names;
	vector<int> gpu_b_height_offsets;
	std::unordered_map<string, deque<long long>> shared_gpu_percent = {
		{"gpu-average", {}},
		{"gpu-vram-total", {}},
		{"gpu-pwr-total", {}},
	};
	long long gpu_pwr_total_max = 0;
}
#endif

namespace Proc {
bool set_priority(pid_t pid, int priority) {
  if (setpriority(PRIO_PROCESS, pid, priority) == 0) {
    return true;
  }
  return false;
}

	void proc_sorter(vector<proc_info>& proc_vec, const string& sorting, bool reverse, bool tree) {
		if (reverse) {
			switch (v_index(sort_vector, sorting)) {
			case 0: rng::stable_sort(proc_vec, rng::less{}, &proc_info::pid); 		break;
			case 1: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::name);		break;
			case 2: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::cmd); 		break;
			case 3: rng::stable_sort(proc_vec, rng::less{}, &proc_info::threads);	break;
			case 4: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::user); 		break;
			case 5: rng::stable_sort(proc_vec, rng::less{}, &proc_info::mem); 		break;
			case 6: rng::stable_sort(proc_vec, rng::less{}, &proc_info::cpu_p);		break;
			case 7: rng::stable_sort(proc_vec, rng::less{}, &proc_info::cpu_c);		break;
			}
		}
		else {
			switch (v_index(sort_vector, sorting)) {
			case 0: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::pid); 		break;
			case 1: rng::stable_sort(proc_vec, rng::less{}, &proc_info::name);		break;
			case 2: rng::stable_sort(proc_vec, rng::less{}, &proc_info::cmd); 		break;
			case 3: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::threads);	break;
			case 4: rng::stable_sort(proc_vec, rng::less{}, &proc_info::user);		break;
			case 5: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::mem); 		break;
			case 6: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::cpu_p);   	break;
			case 7: rng::stable_sort(proc_vec, rng::greater{}, &proc_info::cpu_c);   	break;
			}
		}

		//* When sorting with "cpu lazy" push processes over threshold cpu usage to the front regardless of cumulative usage
		if (not tree and not reverse and sorting == "cpu lazy") {
			double max = 10.0, target = 30.0;
			for (size_t i = 0, x = 0, offset = 0; i < proc_vec.size(); i++) {
				if (i <= 5 and proc_vec.at(i).cpu_p > max)
					max = proc_vec.at(i).cpu_p;
				else if (i == 6)
					target = (max > 30.0) ? max : 10.0;
				if (i == offset and proc_vec.at(i).cpu_p > 30.0)
					offset++;
				else if (proc_vec.at(i).cpu_p > target) {
					rotate(proc_vec.begin() + offset, proc_vec.begin() + i, proc_vec.begin() + i + 1);
					if (++x > 10) break;
				}
			}
		}
	}

	void tree_sort(vector<tree_proc>& proc_vec, const string& sorting, bool reverse, bool paused, int& c_index, const int index_max, bool collapsed) {
		if (proc_vec.size() > 1 and not paused) {
			if (reverse) {
				switch (v_index(sort_vector, sorting)) {
				case 3: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().threads < b.entry.get().threads; });	break;
				case 5: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().mem < b.entry.get().mem; });	break;
				case 6: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().cpu_p < b.entry.get().cpu_p; });	break;
				case 7: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().cpu_c < b.entry.get().cpu_c; });	break;
				}
			}
			else {
				switch (v_index(sort_vector, sorting)) {
				case 3: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().threads > b.entry.get().threads; });	break;
				case 5: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().mem > b.entry.get().mem; });	break;
				case 6: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().cpu_p > b.entry.get().cpu_p; });	break;
				case 7: rng::stable_sort(proc_vec, [](const auto& a, const auto& b) { return a.entry.get().cpu_c > b.entry.get().cpu_c; });	break;
				}
			}
		}

		for (auto& r : proc_vec) {
			r.entry.get().tree_index = (collapsed or r.entry.get().filtered ? index_max : c_index++);
			if (not r.children.empty()) {
				tree_sort(r.children, sorting, reverse, paused, c_index, (collapsed or r.entry.get().collapsed or r.entry.get().tree_index == (size_t)index_max));
			}
		}
	}

	auto matches_filter(const proc_info& proc, const std::string& filter) -> bool {
		if (filter.starts_with("!")) {
			if (filter.size() == 1) {
				return true;
			}

			// An incomplete regex throws, see issue https://github.com/aristocratos/btop/issues/1133
			try {
				std::regex regex { filter.substr(1), std::regex::extended };
				return std::regex_search(std::to_string(proc.pid), regex) || std::regex_search(proc.name, regex) ||
							 std::regex_match(proc.cmd, regex) || std::regex_search(proc.user, regex);
			} catch (std::regex_error& /* unused */) {
				return false;
			}
		}

		return std::to_string(proc.pid).contains(filter) || s_contains_ic(proc.name, filter) ||
					 s_contains_ic(proc.cmd, filter) || s_contains_ic(proc.user, filter);
	}

	void _tree_gen(proc_info& cur_proc, vector<proc_info>& in_procs, vector<tree_proc>& out_procs,
		int cur_depth, bool collapsed, const string& filter, bool found, bool no_update, bool should_filter) {
		bool filtering = false;

		//? If filtering, include children of matching processes
		if (not found and (should_filter or not filter.empty())) {
			if (!matches_filter(cur_proc, filter)) {
				filtering = true;
				cur_proc.filtered = true;
				filter_found++;
			}
			else {
				found = true;
				cur_depth = 0;
			}
		}
		else if (cur_proc.filtered) cur_proc.filtered = false;

		cur_proc.depth = cur_depth;

		//? Set tree index position for process if not filtered out or currently in a collapsed sub-tree
		out_procs.push_back({ cur_proc, {} });
		if (not collapsed and not filtering) {
			cur_proc.tree_index = out_procs.size() - 1;

			//? Try to find name of the binary file and append to program name if not the same
			if (cur_proc.short_cmd.empty() and not cur_proc.cmd.empty()) {
				std::string_view cmd_view = cur_proc.cmd;
				cmd_view = cmd_view.substr((size_t)0, std::min(cmd_view.find(' '), cmd_view.size()));
				cmd_view = cmd_view.substr(std::min(cmd_view.find_last_of('/') + 1, cmd_view.size()));
				cur_proc.short_cmd = string{cmd_view};
			}
		}
		else {
			cur_proc.tree_index = in_procs.size();
		}

		//? Recursive iteration over all children
		for (auto& p : rng::equal_range(in_procs, cur_proc.pid, rng::less{}, &proc_info::ppid)) {
			if (collapsed and not filtering) {
				cur_proc.filtered = true;
			}

			_tree_gen(p, in_procs, out_procs.back().children, cur_depth + 1, (collapsed or cur_proc.collapsed), filter, found, no_update, should_filter);

			if (not no_update and not filtering and (collapsed or cur_proc.collapsed)) {
				//auto& parent = cur_proc;
				if (p.state != 'X') {
					cur_proc.cpu_p += p.cpu_p;
					cur_proc.cpu_c += p.cpu_c;
					cur_proc.mem += p.mem;
					cur_proc.threads += p.threads;
				}
				filter_found++;
				p.filtered = true;
			}
			else if (Config::getB("proc_aggregate") and p.state != 'X') {
				cur_proc.cpu_p += p.cpu_p;
				cur_proc.cpu_c += p.cpu_c;
				cur_proc.mem += p.mem;
				cur_proc.threads += p.threads;
			}
		}
	}

	void _collect_prefixes(tree_proc &t, const bool is_last, const string &header) {
		const bool is_filtered = t.entry.get().filtered;
		if (is_filtered) t.entry.get().depth = 0;

		if (!t.children.empty()) t.entry.get().prefix = header + (t.entry.get().collapsed ? "[+]─": "[-]─");
		else t.entry.get().prefix = header + (is_last ? " └─": " ├─");

		for (auto child = t.children.begin(); child != t.children.end(); ++child) {
			_collect_prefixes(*child, child == (t.children.end() - 1),
				is_filtered ? "": header + (is_last ? "   ": " │ "));
		}
	}

	void toggle_tree_collapse(std::vector<proc_info>& current_procs) {
		//? Build sets of all pids and parent pids to identify root processes
		std::unordered_set<size_t> pid_set, parent_pids;
		for (const auto& p : current_procs) {
			pid_set.insert(p.pid);
			parent_pids.insert(static_cast<size_t>(p.ppid));
		}
		//? If any non-root parent is expanded, collapse; otherwise expand
		const bool do_collapse = rng::any_of(current_procs, [&parent_pids, &pid_set](const proc_info& p) {
			return parent_pids.contains(p.pid)
				and pid_set.contains(static_cast<size_t>(p.ppid))
				and not p.collapsed;
		});
		//? Root processes (parent not in tracked list) are never touched
		for (auto& p : current_procs) {
			if (not pid_set.contains(static_cast<size_t>(p.ppid))) continue;
			p.collapsed = do_collapse;
		}
	}

	void _auto_collapse_oversized(std::vector<proc_info>& current_procs, const bool tree_mode_change) {
		//? Only act when the user just switched into tree view
		const int threshold = Config::getI("proc_tree_auto_collapse");
		if (threshold <= 0 or not tree_mode_change) return;
		//? Never collapse the root process or its direct children, only deeper busy parents
		const size_t root_ppid = static_cast<size_t>(current_procs.at(0).ppid);
		std::unordered_set<size_t> root_pids;
		for (const auto& p : current_procs) {
			if (static_cast<size_t>(p.ppid) == root_ppid) root_pids.insert(p.pid);
		}
		for (auto& p : current_procs) {
			if (static_cast<size_t>(p.ppid) == root_ppid or root_pids.contains(static_cast<size_t>(p.ppid))) continue;
			if (rng::count(current_procs, p.pid, &proc_info::ppid) >= threshold) {
				p.collapsed = true;
			}
		}
	}
}

auto detect_container() -> std::optional<std::string> {
    std::error_code err;

    if (fs::exists(fs::path("/run/.containerenv"), err)) {
        return std::make_optional(std::string { "podman" });
    }
    if (fs::exists(fs::path("/.dockerenv"), err)) {
        return std::make_optional(std::string { "docker" });
    }
    auto systemd_container = fs::path("/run/systemd/container");
    if (fs::exists(systemd_container, err)) {
        auto stream = std::ifstream { systemd_container };
        auto buf = std::string {};
        stream >> buf;
        return std::make_optional(buf);
    }

    return std::nullopt;
}

#if defined(GPU_SUPPORT)
const array<string, 2> Gpu::mem_names { "used", "free" };
#endif

const vector<string> Proc::sort_vector = {
	"pid",
	"name",
	"command",
	"threads",
	"user",
	"memory",
	"cpu direct",
	"cpu lazy",
};

const std::unordered_map<char, string> Proc::proc_states = {
	{'R', "Running"},
	{'S', "Sleeping"},
	{'D', "Waiting"},
	{'Z', "Zombie"},
	{'T', "Stopped"},
	{'t', "Tracing"},
	{'X', "Dead"},
	{'x', "Dead"},
	{'K', "Wakekill"},
	{'W', "Unknown"},
	{'P', "Parked"}
};
