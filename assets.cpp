#include "assets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#include "arena.h"
#include "sqlite3.h"

namespace fs = std::filesystem;

namespace solver {
namespace {

constexpr uint32_t TYPE_LINK = 0x4000000;

// EDOPro treats an empty cards.cdb as missing; sqlite would open it without
// error and return zero cards, which would hide the problem.
bool UsableFile(const fs::path& p) {
	std::error_code ec;
	return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0;
}

} // namespace

bool CardDB::LoadFile(const std::string& path) {
	sqlite3* db = nullptr;
	if(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
		if(db)
			sqlite3_close(db);
		return false;
	}
	static constexpr char kQuery[] =
		"SELECT id,alias,setcode,type,atk,def,level,race,attribute "
				"FROM datas";
	sqlite3_stmt* st = nullptr;
	if(sqlite3_prepare_v2(db, kQuery, -1, &st, nullptr) != SQLITE_OK) {
		sqlite3_close(db);
		return false;
	}
	size_t n = 0;
	while(sqlite3_step(st) == SQLITE_ROW) {
		CardRow row;
		row.code = static_cast<uint32_t>(sqlite3_column_int64(st, 0));
		row.alias = static_cast<uint32_t>(sqlite3_column_int64(st, 1));
		uint64_t setcode = static_cast<uint64_t>(sqlite3_column_int64(st, 2));
		row.type = static_cast<uint32_t>(sqlite3_column_int64(st, 3));
		row.attack = sqlite3_column_int(st, 4);
		row.defense = sqlite3_column_int(st, 5);
		int level = sqlite3_column_int(st, 6);
		row.race = static_cast<uint64_t>(sqlite3_column_int64(st, 7));
		row.attribute = static_cast<uint32_t>(sqlite3_column_int64(st, 8));

		for(int i = 0; i < 4; ++i) {
			uint16_t sc = static_cast<uint16_t>((setcode >> (i * 16)) & 0xffff);
			if(sc)
				row.setcodes.push_back(sc);
		}
		if(!row.setcodes.empty())
			row.setcodes.push_back(0);

		// Link monsters keep their arrows in the def column.
		if(row.type & TYPE_LINK) {
			row.link_marker = static_cast<uint32_t>(row.defense);
			row.defense = 0;
		}
		// Negative levels ("?" cards): data_manager.cpp:145.
		row.level = (level < 0) ? static_cast<uint32_t>(-(level & 0xff))
								: static_cast<uint32_t>(level & 0xff);
		row.lscale = (level >> 24) & 0xff;
		row.rscale = (level >> 16) & 0xff;

		cards[row.code] = std::move(row);
		++n;
	}
	sqlite3_finalize(st);

	// Names are only used in reports: a missing one is not an error.
	sqlite3_stmt* ns = nullptr;
	if(sqlite3_prepare_v2(db, "SELECT id,name,desc FROM texts", -1, &ns,
						  nullptr) == SQLITE_OK) {
		while(sqlite3_step(ns) == SQLITE_ROW) {
			const uint32_t id = static_cast<uint32_t>(sqlite3_column_int64(ns, 0));
			const unsigned char* nm = sqlite3_column_text(ns, 1);
			if(nm && *nm)
				names[id] = reinterpret_cast<const char*>(nm);
			// FIRST LINE of the text: for an extra deck monster that is the MATERIALS
			// line, and that is all the recipe graph bootstrap needs. The rest is
			// dropped, because full card text weighs hundreds of megabytes across all
			// the databases.
			const unsigned char* ds = sqlite3_column_text(ns, 2);
			if(ds && *ds) {
				std::string d = reinterpret_cast<const char*>(ds);
				const size_t nl = d.find('\n');
				if(nl != std::string::npos)
					d.resize(nl);
				while(!d.empty() && (d.back() == '\r' || d.back() == ' '))
					d.pop_back();
				if(!d.empty())
					material_lines[id] = std::move(d);
			}
		}
		sqlite3_finalize(ns);
	}

	sqlite3_close(db);
	if(n)
		sources.push_back(path);
	return n > 0;
}

const std::string& CardDB::Name(uint32_t code) const {
	static const std::string kUnknown = "?";
	auto it = names.find(code);
	return it == names.end() ? kUnknown : it->second;
}

std::vector<std::pair<uint32_t, std::string>>
CardDB::FindByName(const std::string& needle) const {
	auto lower = [](std::string s) {
		for(char& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	};
	const std::string want = lower(needle);
	std::vector<std::pair<uint32_t, std::string>> out;
	if(want.empty())
		return out;
	for(const auto& [code, name] : names) {
		if(lower(name).find(want) == std::string::npos)
			continue;
		uint32_t canon = Canonical(code);
		bool dup = false;
		for(const auto& [c, n] : out)
			if(c == canon) { dup = true; break; }
		if(!dup)
			out.emplace_back(canon, name);
	}
	std::sort(out.begin(), out.end());
	return out;
}

bool CardDB::Load(const std::string& workdir, std::string& error) {
	std::error_code ec;
	std::vector<fs::path> paths;

	fs::path root_cdb = fs::path(workdir) / "cards.cdb";
	if(UsableFile(root_cdb))
		paths.push_back(root_cdb);

	for(const char* sub : { "expansions", "repositories" }) {
		fs::path dir = fs::path(workdir) / sub;
		if(!fs::is_directory(dir, ec))
			continue;
		for(auto it = fs::recursive_directory_iterator(
				dir, fs::directory_options::skip_permission_denied, ec);
			it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if(ec)
				break;
			if(it->is_regular_file(ec) && it->path().extension() == ".cdb")
				paths.push_back(it->path());
		}
	}
	std::sort(paths.begin(), paths.end());
	for(const auto& p : paths)
		LoadFile(p.string());

	// Exact name -> canonical code index. Two artworks of the same card share a
	// name: we keep the canonical code, otherwise a material named in text would
	// designate one printing rather than a card.
	for(const auto& [code, name] : names) {
		const uint32_t canon = Canonical(code);
		by_name.emplace(name, canon);
	}

	if(cards.empty()) {
		error = "no card loaded from " + workdir +
				" (expansions/*.cdb and repositories/**/*.cdb "
								"not found or empty)";
		return false;
	}
	return true;
}

const CardRow* CardDB::Find(uint32_t code) const {
	auto it = cards.find(code);
	return it == cards.end() ? nullptr : &it->second;
}

const std::string& CardDB::MaterialLine(uint32_t code) const {
	static const std::string kEmpty;
	auto it = material_lines.find(code);
	return it == material_lines.end() ? kEmpty : it->second;
}

uint32_t CardDB::CodeByExactName(const std::string& name) const {
	auto it = by_name.find(name);
	return it == by_name.end() ? 0u : it->second;
}

void CardDB::NoteUnknown(uint32_t code) const {
	std::lock_guard<std::mutex> lk(unknown_mx);
	unknown.insert(code);
}

std::vector<uint32_t> CardDB::UnknownCodes() const {
	std::lock_guard<std::mutex> lk(unknown_mx);
	return std::vector<uint32_t>(unknown.begin(), unknown.end());
}

uint32_t CardDB::Canonical(uint32_t code) const {
	const CardRow* r = Find(code);
	return (r && r->alias) ? r->alias : code;
}

void ScriptProvider::Init(const std::string& wd,
						  const std::vector<std::string>& override_roots) {
	workdir = wd;
	std::error_code ec;

	// A script directory and its immediate subdirectories.
	auto expand = [&](const fs::path& root) {
		if(!fs::is_directory(root, ec))
			return;
		dirs.push_back(root.string());
		std::vector<fs::path> subs;
		for(const auto& e : fs::directory_iterator(root, ec)) {
			if(e.is_directory(ec) && e.path().filename().string().front() != '.')
				subs.push_back(e.path());
		}
		std::sort(subs.begin(), subs.end());
		for(const auto& s : subs)
			dirs.push_back(s.string());
	};

	// Repositories come before the base installation: many cards exist ONLY in
	// the repository (game.cpp:2959).
	if(!override_roots.empty()) {
		for(const auto& r : override_roots)
			expand(r);
	} else {
		fs::path repos = fs::path(workdir) / "repositories";
		if(fs::is_directory(repos, ec)) {
			std::vector<fs::path> list;
			for(const auto& e : fs::directory_iterator(repos, ec))
				if(e.is_directory(ec))
					list.push_back(e.path() / "script");
			std::sort(list.begin(), list.end());
			for(const auto& r : list)
				expand(r);
		}
	}
	expand(fs::path(workdir) / "expansions" / "script");
	expand(fs::path(workdir) / "script");
}

std::vector<char> ScriptProvider::Read(const std::string& raw_name) {
	// Nothing this function does belongs to the duel: neither the returned buffer
	// nor, above all, the miss log, which is SHARED between workers. Letting it
	// allocate in the calling thread's arena would later free, from another
	// thread, a pointer no arena recognises.
	ArenaPause off;

	std::string name = raw_name;
	std::replace(name.begin(), name.end(), '\\', '/');
	while(name.rfind("./", 0) == 0)
		name.erase(0, 2);

	// Cache first: a script already read pays neither the directory sweep nor the
	// I/O again. The copy is returned by value; the cached buffer belongs to the
	// host (ArenaPause), and the caller owns its own.
	{
		std::lock_guard<std::mutex> lock(misses_mutex);
		auto it = cache.find(name);
		if(it != cache.end()) {
			++cache_hits;
			return it->second;
		}
	}

	// `bad_read` separates "the file is not there" from "the file is there and I
	// could not read it". Conflating the two made the loader fall through to the
	// NEXT repository and load the SAME card from another version of the script
	// set: an I/O error manufactured exactly the script-set mismatch this project
	// fears most, without ever reaching the `misses` set that would have
	// reported it.
	bool bad_read = false;
	std::string bad_path;
	auto slurp = [&](const fs::path& p) -> std::vector<char> {
		FILE* fp = std::fopen(p.string().c_str(), "rb");
		if(!fp)
			return {};
		std::fseek(fp, 0, SEEK_END);
		long size = std::ftell(fp);
		std::fseek(fp, 0, SEEK_SET);
		std::vector<char> buf(size > 0 ? static_cast<size_t>(size) : 0);
		if(buf.empty() ||
		   std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
			// Empty file OR short read: either way the file EXISTS and does not give up
			// its contents. We do not fall through.
			buf.clear();
			bad_read = true;
			bad_path = p.string();
		}
		std::fclose(fp);
		// UTF-8 BOM: Lua does not tolerate one at the head of a chunk.
		if(buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
		   static_cast<unsigned char>(buf[1]) == 0xBB &&
		   static_cast<unsigned char>(buf[2]) == 0xBF)
			buf.erase(buf.begin(), buf.begin() + 3);
		return buf;
	};

	auto remember = [&](const std::vector<char>& buf) {
		std::lock_guard<std::mutex> lock(misses_mutex);
		cache.emplace(name, buf);
	};
	std::error_code ec;
	for(const auto& d : dirs) {
		fs::path p = fs::path(d) / name;
		if(fs::is_regular_file(p, ec)) {
			auto buf = slurp(p);
			if(!buf.empty()) {
				remember(buf);
				return buf;
			}
			if(bad_read)
				break;   // do NOT fall through to another version
		}
	}
	if(!bad_read) {
		fs::path direct = fs::path(workdir) / name;
		if(fs::is_regular_file(direct, ec)) {
			auto buf = slurp(direct);
			if(!buf.empty()) {
				remember(buf);
				return buf;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(misses_mutex);
		if(bad_read)
			unreadable.insert(bad_path);
		else
			misses.insert(name);
	}
	return {};
}

} // namespace solver
