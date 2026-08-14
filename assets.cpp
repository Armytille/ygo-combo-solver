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

// EDOPro traite un cards.cdb vide comme absent ; sqlite l'ouvrirait sans
// erreur et renverrait zero carte, ce qui masquerait le probleme.
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
		"SELECT id,alias,setcode,type,atk,def,level,race,attribute FROM datas";
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

		// Les monstres Lien rangent leurs fleches dans la colonne def.
		if(row.type & TYPE_LINK) {
			row.link_marker = static_cast<uint32_t>(row.defense);
			row.defense = 0;
		}
		// Niveaux negatifs (cartes "?") : data_manager.cpp:145.
		row.level = (level < 0) ? static_cast<uint32_t>(-(level & 0xff))
								: static_cast<uint32_t>(level & 0xff);
		row.lscale = (level >> 24) & 0xff;
		row.rscale = (level >> 16) & 0xff;

		cards[row.code] = std::move(row);
		++n;
	}
	sqlite3_finalize(st);

	// Les noms ne servent qu'aux rapports : leur absence n'est pas une erreur.
	sqlite3_stmt* ns = nullptr;
	if(sqlite3_prepare_v2(db, "SELECT id,name,desc FROM texts", -1, &ns,
						  nullptr) == SQLITE_OK) {
		while(sqlite3_step(ns) == SQLITE_ROW) {
			const uint32_t id = static_cast<uint32_t>(sqlite3_column_int64(ns, 0));
			const unsigned char* nm = sqlite3_column_text(ns, 1);
			if(nm && *nm)
				names[id] = reinterpret_cast<const char*>(nm);
			// PREMIERE LIGNE du texte : pour un monstre d'extra deck c'est la
			// ligne de MATERIAUX, et c'est tout ce dont l'amorce du graphe de
			// recettes a besoin. On ne garde pas le reste : le texte complet
			// pese des centaines de Mo sur l'ensemble des bases.
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

	// Index nom EXACT -> code canonique. Deux illustrations de la meme carte
	// portent le meme nom : on garde le code canonique, sinon un materiau nomme
	// par le texte designerait un exemplaire et non une carte.
	for(const auto& [code, name] : names) {
		const uint32_t canon = Canonical(code);
		by_name.emplace(name, canon);
	}

	if(cards.empty()) {
		error = "aucune carte chargee depuis " + workdir +
				" (expansions/*.cdb et repositories/**/*.cdb introuvables ou vides)";
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

	// Un dossier de scripts et ses sous-dossiers directs.
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

	// Les depots passent devant l'installation de base : beaucoup de cartes
	// n'existent QUE dans le depot (game.cpp:2959).
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
	// Rien de ce que fait cette fonction n'appartient au duel : ni le tampon
	// rendu, ni surtout le journal des manquants, PARTAGE entre workers. Le
	// laisser allouer dans l'arene du thread appelant ferait liberer plus tard,
	// depuis un autre thread, un pointeur qu'aucune arene ne reconnait.
	ArenaPause off;

	std::string name = raw_name;
	std::replace(name.begin(), name.end(), '\\', '/');
	while(name.rfind("./", 0) == 0)
		name.erase(0, 2);

	// `bad_read` distingue « le fichier n'est pas la » de « le fichier est la et
	// je n'ai pas su le lire ». Confondre les deux faisait tomber le chargeur au
	// depot SUIVANT et charger la MEME carte depuis une autre version du jeu de
	// scripts : une erreur d'E/S fabriquait exactement le decalage de jeux de
	// scripts que ce depot redoute le plus, sans jamais atteindre l'ensemble
	// `misses` qui l'aurait signale (4.7).
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
			// Fichier vide OU lecture courte : dans les deux cas le fichier
			// EXISTE et ne donne pas son contenu. On ne se rabat pas.
			buf.clear();
			bad_read = true;
			bad_path = p.string();
		}
		std::fclose(fp);
		// BOM UTF-8 : Lua ne le tolere pas en tete de chunk.
		if(buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
		   static_cast<unsigned char>(buf[1]) == 0xBB &&
		   static_cast<unsigned char>(buf[2]) == 0xBF)
			buf.erase(buf.begin(), buf.begin() + 3);
		return buf;
	};

	std::error_code ec;
	for(const auto& d : dirs) {
		fs::path p = fs::path(d) / name;
		if(fs::is_regular_file(p, ec)) {
			auto buf = slurp(p);
			if(!buf.empty())
				return buf;
			if(bad_read)
				break;   // ne PAS se rabattre sur une autre version
		}
	}
	if(!bad_read) {
		fs::path direct = fs::path(workdir) / name;
		if(fs::is_regular_file(direct, ec)) {
			auto buf = slurp(direct);
			if(!buf.empty())
				return buf;
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
