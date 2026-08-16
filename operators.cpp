#include "operators.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>

namespace solver {
namespace {

// --- OUTILLAGE DE LECTURE ----------------------------------------------------

bool IsIdentChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string Trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while(a < b && std::isspace(static_cast<unsigned char>(s[a])))
		++a;
	while(b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		--b;
	return s.substr(a, b - a);
}

// Retire les commentaires Lua d'une ligne. Les blocs `--[[ ]]` sont geres par
// l'appelant (ils traversent les lignes) ; ici, seul le `--` de fin de ligne.
// Les chaines litterales sont respectees : un `--` dans "a--b" n'en est pas un.
std::string StripComment(const std::string& line) {
	bool in_str = false;
	char q = 0;
	for(size_t i = 0; i + 1 < line.size(); ++i) {
		if(in_str) {
			if(line[i] == '\\') { ++i; continue; }
			if(line[i] == q)
				in_str = false;
			continue;
		}
		if(line[i] == '"' || line[i] == '\'') {
			in_str = true;
			q = line[i];
			continue;
		}
		if(line[i] == '-' && line[i + 1] == '-')
			return line.substr(0, i);
	}
	return line;
}

// Texte entre la parenthese ouvrante qui suit `pos` et sa fermante. Rend faux si
// la parenthese ne se ferme pas sur la ligne — auquel cas on ne devine rien.
bool ParenArgs(const std::string& s, size_t pos, std::string& out,
			   size_t* end = nullptr) {
	size_t open = s.find('(', pos);
	if(open == std::string::npos)
		return false;
	int depth = 0;
	bool in_str = false;
	char q = 0;
	for(size_t i = open; i < s.size(); ++i) {
		const char c = s[i];
		if(in_str) {
			if(c == '\\') { ++i; continue; }
			if(c == q) in_str = false;
			continue;
		}
		if(c == '"' || c == '\'') { in_str = true; q = c; continue; }
		if(c == '(' || c == '{' || c == '[') ++depth;
		else if(c == ')' || c == '}' || c == ']') {
			--depth;
			if(depth == 0) {
				out = s.substr(open + 1, i - open - 1);
				if(end) *end = i;
				return true;
			}
		}
	}
	return false;
}

// Decoupe une liste d'arguments sur les virgules DE PREMIER NIVEAU.
std::vector<std::string> SplitArgs(const std::string& s) {
	std::vector<std::string> out;
	int depth = 0;
	bool in_str = false;
	char q = 0;
	size_t start = 0;
	for(size_t i = 0; i < s.size(); ++i) {
		const char c = s[i];
		if(in_str) {
			if(c == '\\') { ++i; continue; }
			if(c == q) in_str = false;
			continue;
		}
		if(c == '"' || c == '\'') { in_str = true; q = c; continue; }
		if(c == '(' || c == '{' || c == '[') ++depth;
		else if(c == ')' || c == '}' || c == ']') --depth;
		else if(c == ',' && depth == 0) {
			out.push_back(Trim(s.substr(start, i - start)));
			start = i + 1;
		}
	}
	out.push_back(Trim(s.substr(start)));
	return out;
}

// Occurrence d'un appel `name(` en dehors de toute chaine, a partir de `from`.
size_t FindCall(const std::string& s, const std::string& name, size_t from = 0) {
	while(from < s.size()) {
		size_t p = s.find(name, from);
		if(p == std::string::npos)
			return std::string::npos;
		// Un appel n'est pas un suffixe d'identifiant : `xSetRange` n'est pas
		// `SetRange`. On exige que le caractere precedent ne soit pas un
		// caractere d'identifiant, sauf quand le nom commence par `:` ou `.`.
		const bool boundary_ok =
			p == 0 || !IsIdentChar(s[p - 1]) || name[0] == ':' || name[0] == '.';
		size_t after = p + name.size();
		while(after < s.size() && std::isspace(static_cast<unsigned char>(s[after])))
			++after;
		if(boundary_ok && after < s.size() && s[after] == '(')
			return p;
		from = p + 1;
	}
	return std::string::npos;
}

} // namespace

// --- CONSTANTES --------------------------------------------------------------

void ConstantTable::Absorb(const std::vector<char>& src) {
	std::string all(src.begin(), src.end());
	bool in_block = false;
	size_t pos = 0;
	while(pos <= all.size()) {
		size_t nl = all.find('\n', pos);
		std::string line = all.substr(pos, nl == std::string::npos
											   ? std::string::npos : nl - pos);
		pos = nl == std::string::npos ? all.size() + 1 : nl + 1;
		if(in_block) {
			size_t e = line.find("]]");
			if(e == std::string::npos)
				continue;
			line = line.substr(e + 2);
			in_block = false;
		}
		size_t b = line.find("--[[");
		if(b != std::string::npos) {
			std::string head = line.substr(0, b);
			size_t e = line.find("]]", b);
			if(e == std::string::npos) {
				in_block = true;
				line = head;
			} else {
				line = head + line.substr(e + 2);
			}
		}
		line = StripComment(line);
		size_t eq = line.find('=');
		if(eq == std::string::npos || eq == 0)
			continue;
		// Ni `==`, ni `<=`, ni `~=` : ce fichier ne contient que des
		// affectations, mais on ne veut pas d'un faux positif silencieux.
		if(eq + 1 < line.size() && line[eq + 1] == '=')
			continue;
		if(line[eq - 1] == '=' || line[eq - 1] == '<' || line[eq - 1] == '>' ||
		   line[eq - 1] == '~' || line[eq - 1] == '!')
			continue;
		std::string name = Trim(line.substr(0, eq));
		if(name.empty() || std::isdigit(static_cast<unsigned char>(name[0])))
			continue;
		bool ident = true;
		for(char c : name)
			if(!IsIdentChar(c)) { ident = false; break; }
		if(!ident)
			continue;
		uint64_t v = 0;
		if(!Eval(Trim(line.substr(eq + 1)), v))
			continue;
		if(vals.emplace(name, v).second)
			order.emplace_back(name, v);
	}
}

// `function Auxiliary.Stringid(code,id) return (id&0xfffff)|code<<20 end`
// On y lit DEUX choses : de combien le code est decale, et sur combien de bits
// l'index tient. Rien d'autre du corps n'est interprete — si la forme change,
// `string_shift_read` reste faux et l'appelant le DIT au lieu de supposer.
void ConstantTable::ReadStringId(const std::vector<char>& src) {
	const std::string all(src.begin(), src.end());
	const size_t f = all.find("function Auxiliary.Stringid");
	if(f == std::string::npos)
		return;
	const size_t e = all.find("\nend", f);
	const std::string body =
		all.substr(f, e == std::string::npos ? 200 : e - f);
	const size_t sh = body.find("<<");
	if(sh == std::string::npos)
		return;
	size_t i = sh + 2;
	while(i < body.size() && std::isspace(static_cast<unsigned char>(body[i])))
		++i;
	size_t j = i;
	while(j < body.size() && std::isdigit(static_cast<unsigned char>(body[j])))
		++j;
	if(j == i)
		return;
	string_shift = static_cast<uint32_t>(std::strtoul(body.substr(i, j - i).c_str(),
													  nullptr, 10));
	string_mask = string_shift >= 64 ? ~0ull : ((1ull << string_shift) - 1);
	// Le masque explicite, quand il est ecrit, prime : `id & 0xfffff`.
	const size_t mp = body.find("&0x");
	if(mp != std::string::npos)
		string_mask = std::strtoull(body.c_str() + mp + 3, nullptr, 16);
	string_shift_read = true;
}

uint64_t ConstantTable::StringId(uint32_t code, uint32_t index) const {
	return (static_cast<uint64_t>(index) & string_mask) |
		   (static_cast<uint64_t>(code) << string_shift);
}

size_t ConstantTable::Load(ScriptProvider& sp) {
	// L'ORDRE COMPTE : `constant.lua` d'abord, parce que les autres fichiers y
	// puisent (une valeur composee ne s'evalue que si ses termes sont deja la).
	for(const char* f : { "constant.lua", "archetype_setcode_constants.lua",
						  "card_counter_constants.lua" }) {
		std::vector<char> src = sp.Read(f);
		if(src.empty())
			continue;
		files.push_back(f);
		Absorb(src);
	}
	if(std::vector<char> u = sp.Read("utility.lua"); !u.empty()) {
		files.push_back("utility.lua");
		ReadStringId(u);
	}
	return vals.size();
}

bool ConstantTable::Lookup(const std::string& name, uint64_t& out) const {
	auto it = vals.find(name);
	if(it == vals.end())
		return false;
	out = it->second;
	return true;
}

bool ConstantTable::Eval(const std::string& expr, uint64_t& out) const {
	// Grammaire volontairement PAUVRE : jetons separes par `+`, `|`, `~`
	// (concatenation de drapeaux) et rien d'autre. Une expression qui sort de
	// cette grammaire est REFUSEE, jamais approximee — une valeur inventee
	// fabriquerait un operateur qui n'existe pas.
	std::string s = Trim(expr);
	if(s.empty())
		return false;
	// Une virgule ou un appel : hors grammaire.
	if(s.find('(') != std::string::npos || s.find(',') != std::string::npos ||
	   s.find('{') != std::string::npos)
		return false;
	uint64_t acc = 0;
	size_t i = 0;
	bool any = false;
	while(i < s.size()) {
		while(i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) ||
							   s[i] == '+' || s[i] == '|'))
			++i;
		if(i >= s.size())
			break;
		size_t j = i;
		while(j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) &&
			  s[j] != '+' && s[j] != '|')
			++j;
		std::string tok = s.substr(i, j - i);
		i = j;
		if(tok.empty())
			continue;
		uint64_t v = 0;
		if(tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
			char* endp = nullptr;
			v = std::strtoull(tok.c_str() + 2, &endp, 16);
			if(!endp || *endp)
				return false;
		} else if(std::isdigit(static_cast<unsigned char>(tok[0]))) {
			char* endp = nullptr;
			v = std::strtoull(tok.c_str(), &endp, 10);
			if(!endp || *endp)
				return false;
		} else if(!Lookup(tok, v)) {
			return false;
		}
		acc |= v;
		any = true;
	}
	if(!any)
		return false;
	out = acc;
	return true;
}

std::string ConstantTable::NameOf(const std::string& prefix,
								  uint64_t value) const {
	for(const auto& [n, v] : order)
		if(v == value && n.compare(0, prefix.size(), prefix) == 0)
			return n;
	return {};
}

std::string ConstantTable::MaskNames(const std::string& prefix,
									 uint64_t value) const {
	if(!value)
		return "-";
	std::string exact = NameOf(prefix, value);
	if(!exact.empty())
		return exact;
	std::string out;
	uint64_t left = value;
	for(const auto& [n, v] : order) {
		if(!v || n.compare(0, prefix.size(), prefix) != 0)
			continue;
		// Un seul bit : c'est un drapeau, pas un alias compose.
		if(v & (v - 1))
			continue;
		if((left & v) == v) {
			if(!out.empty())
				out += "|";
			out += n;
			left &= ~v;
		}
	}
	if(left) {
		char buf[32];
		std::snprintf(buf, sizeof buf, "%s0x%llx", out.empty() ? "" : "|",
					  (unsigned long long)left);
		out += buf;
	}
	return out.empty() ? "-" : out;
}

// --- ZONES SYMBOLIQUES -------------------------------------------------------

uint64_t NormalizeRange(uint64_t range) {
	if(!range)
		return 0;
	uint64_t out = range & 0xffu;   // les zones physiques passent telles quelles
	if(range & 0x100u) out |= 0x8u;   // FZONE  -> SZONE
	if(range & 0x200u) out |= 0x8u;   // PZONE  -> SZONE
	if(range & 0x400u) out |= 0x8u;   // STZONE -> SZONE
	if(range & 0x800u) out |= 0x4u;   // MMZONE -> MZONE
	if(range & 0x1000u) out |= 0x4u;  // EMZONE -> MZONE
	return out;
}

// --- ANALYSE STATIQUE D'UN SCRIPT --------------------------------------------

CardOperators ParseScript(uint32_t code, const std::vector<char>& src,
						  const ConstantTable& kt,
						  const std::string& init_fn) {
	CardOperators co;
	co.code = code;
	co.script_found = !src.empty();
	if(src.empty())
		return co;

	std::string all(src.begin(), src.end());
	std::vector<std::string> lines;
	{
		size_t pos = 0;
		while(pos <= all.size()) {
			size_t nl = all.find('\n', pos);
			lines.push_back(all.substr(pos, nl == std::string::npos
											   ? std::string::npos : nl - pos));
			if(nl == std::string::npos)
				break;
			pos = nl + 1;
		}
	}

	// `local s,id=GetID()` : `id` EST le code de la carte. On ne le suppose pas,
	// on le lit — un script partage (alias, variantes) le dirait autrement.
	uint32_t script_id = code;

	std::vector<DeclaredEffect> effects;
	std::unordered_map<std::string, size_t> binding;   // var -> index
	std::string cur_fn = "<chunk>";
	bool in_block_comment = false;

	auto eval_or_zero = [&](const std::string& e) -> uint64_t {
		uint64_t v = 0;
		return kt.Eval(e, v) ? v : 0;
	};

	for(size_t ln = 0; ln < lines.size(); ++ln) {
		std::string line = lines[ln];
		if(in_block_comment) {
			size_t e = line.find("]]");
			if(e == std::string::npos)
				continue;
			line = line.substr(e + 2);
			in_block_comment = false;
		}
		{
			size_t b = line.find("--[[");
			if(b != std::string::npos) {
				std::string head = line.substr(0, b);
				size_t e = line.find("]]", b);
				if(e == std::string::npos) {
					in_block_comment = true;
					line = head;
				} else {
					line = head + line.substr(e + 2);
				}
			}
		}
		line = StripComment(line);
		if(Trim(line).empty())
			continue;
		const int lineno = static_cast<int>(ln + 1);

		// Contexte de fonction. `function s.NAME(` ouvre, un `end` en colonne 0
		// ferme. C'est grossier et c'est SUFFISANT : les scripts de cartes
		// n'imbriquent pas de fonction nommee au premier niveau.
		//
		// LE NOM RESTE QUALIFIE hors des scripts de cartes. Un script de carte
		// prefixe tout par `s.` (ou `cXXXX.`) et l'on retire ce prefixe ; un
		// fichier `proc_*.lua` declare `Pendulum.AddProcedure`, et c'est ce nom
		// ENTIER que la carte appelle — le tronquer confondrait les procedures
		// homonymes de deux familles d'invocation.
		{
			size_t fp = line.find("function ");
			size_t par = fp == std::string::npos ? std::string::npos
												 : line.find('(', fp);
			if(fp != std::string::npos && par != std::string::npos) {
				std::string full = Trim(line.substr(fp + 9, par - fp - 9));
				if(full.compare(0, 2, "s.") == 0)
					full = full.substr(2);
				else if(full.size() > 2 && full[0] == 'c' &&
						std::isdigit(static_cast<unsigned char>(full[1]))) {
					size_t d = full.find('.');
					if(d != std::string::npos)
						full = full.substr(d + 1);
				}
				if(!full.empty())
					cur_fn = full;
			} else if(!line.empty() && line[0] == 'e' && Trim(line) == "end") {
				cur_fn = "<chunk>";
			} else {
				// `Pendulum.AddProcedure = aux.FunctionWithNamedArgs(function(c,...)`
				// : une procedure declaree par AFFECTATION. Sans ce cas, les
				// operateurs des procedures seraient invisibles. La recherche
				// est INSENSIBLE A LA CASSE : le fabricant s'appelle
				// `FunctionWithNamedArgs`, et un `find("function")` sensible a
				// la casse manquait TOUTES les procedures ainsi declarees.
				size_t eq = line.find('=');
				std::string low = line;
				std::transform(low.begin(), low.end(), low.begin(), [](char ch) {
					return static_cast<char>(
						std::tolower(static_cast<unsigned char>(ch)));
				});
				if(eq != std::string::npos && eq + 1 < line.size() &&
				   line[eq + 1] != '=' &&
				   low.find("function", eq) != std::string::npos) {
					std::string lhs = Trim(line.substr(0, eq));
					if(lhs.find('.') != std::string::npos &&
					   lhs.find(' ') == std::string::npos &&
					   lhs.find(':') == std::string::npos &&
					   lhs.find('[') == std::string::npos)
						cur_fn = lhs;
				}
			}
		}

		// `local s,id=GetID()`
		if(FindCall(line, "GetID") != std::string::npos) {
			// rien a extraire : `id` vaut le code du fichier par construction
			(void)script_id;
		}

		// --- creation d'un effet ---------------------------------------------
		size_t cep = FindCall(line, "Effect.CreateEffect");
		if(cep != std::string::npos) {
			size_t eq = line.rfind('=', cep);
			if(eq != std::string::npos) {
				std::string lhs = Trim(line.substr(0, eq));
				if(lhs.compare(0, 6, "local ") == 0)
					lhs = Trim(lhs.substr(6));
				if(!lhs.empty() && lhs.find(',') == std::string::npos) {
					DeclaredEffect de;
					de.var = lhs;
					de.in_function = cur_fn;
					de.line = lineno;
					de.at_init = cur_fn == init_fn;
					effects.push_back(de);
					binding[lhs] = effects.size() - 1;
				}
			}
		}
		// `local e2=e1:Clone()` — un clone HERITE de tout, y compris de la
		// description. L'ignorer perdrait des operateurs entiers (Leo en a un).
		{
			size_t cp = line.find(":Clone()");
			if(cp != std::string::npos) {
				size_t eq = line.rfind('=', cp);
				if(eq != std::string::npos) {
					size_t vs = cp;
					while(vs > 0 && IsIdentChar(line[vs - 1]))
						--vs;
					std::string srcvar = line.substr(vs, cp - vs);
					std::string lhs = Trim(line.substr(0, eq));
					if(lhs.compare(0, 6, "local ") == 0)
						lhs = Trim(lhs.substr(6));
					auto it = binding.find(srcvar);
					if(it != binding.end() && !lhs.empty()) {
						DeclaredEffect de = effects[it->second];
						de.var = lhs;
						de.in_function = cur_fn;
						de.line = lineno;
						de.registered = false;
						de.at_init = cur_fn == init_fn;
						effects.push_back(de);
						binding[lhs] = effects.size() - 1;
					}
				}
			}
		}

		// --- setters sur un effet connu --------------------------------------
		for(auto& [var, idx] : binding) {
			const std::string pat = var + ":Set";
			size_t p = line.find(pat);
			if(p == std::string::npos)
				continue;
			if(p > 0 && IsIdentChar(line[p - 1]))
				continue;
			size_t name_start = p + var.size() + 1;
			size_t name_end = line.find('(', name_start);
			if(name_end == std::string::npos)
				continue;
			const std::string setter =
				Trim(line.substr(name_start, name_end - name_start));
			std::string args;
			if(!ParenArgs(line, name_start, args))
				continue;
			std::vector<std::string> a = SplitArgs(args);
			DeclaredEffect& de = effects[idx];
			if(setter == "SetType")
				de.etype = eval_or_zero(args);
			else if(setter == "SetRange")
				de.range = eval_or_zero(args);
			else if(setter == "SetProperty")
				de.property = eval_or_zero(args);
			else if(setter == "SetCategory")
				de.category = eval_or_zero(args);
			else if(setter == "SetTargetRange" && a.size() >= 2) {
				de.target_range_self = eval_or_zero(a[0]);
				de.target_range_opp = eval_or_zero(a[1]);
			} else if(setter == "SetCode") {
				de.code_name = Trim(args);
				de.code_value = eval_or_zero(args);
				de.code_is_event = de.code_name.compare(0, 6, "EVENT_") == 0;
				de.code_is_effect = de.code_name.compare(0, 7, "EFFECT_") == 0;
			} else if(setter == "SetCountLimit" && !a.empty()) {
				de.has_count_limit = true;
				uint64_t n = 0;
				de.count_limit = kt.Eval(a[0], n) ? static_cast<uint32_t>(n) : 1u;
				if(a.size() >= 2) {
					de.count_by_name = true;
					de.count_tag = a[1];
				}
			} else if(setter == "SetCost")
				de.fn_cost = Trim(args);
			else if(setter == "SetCondition")
				de.fn_condition = Trim(args);
			else if(setter == "SetTarget")
				de.fn_target = Trim(args);
			else if(setter == "SetOperation")
				de.fn_operation = Trim(args);
			else if(setter == "SetValue")
				de.fn_value = Trim(args);
			else if(setter == "SetDescription") {
				de.has_desc = true;
				size_t sp = FindCall(args, "aux.Stringid");
				if(sp != std::string::npos) {
					std::string sargs;
					if(ParenArgs(args, sp, sargs)) {
						std::vector<std::string> sa = SplitArgs(sargs);
						if(sa.size() >= 2) {
							uint64_t cid = 0, n = 0;
							if(sa[0] == "id")
								cid = code;
							else if(!kt.Eval(sa[0], cid))
								cid = 0;
							kt.Eval(sa[1], n);
							de.desc_card = static_cast<uint32_t>(cid);
							de.desc_index = static_cast<uint32_t>(n);
							de.desc_value = kt.StringId(static_cast<uint32_t>(cid), static_cast<uint32_t>(n));
						}
					}
				} else {
					uint64_t v = 0;
					if(kt.Eval(args, v)) {
						de.desc_value = v;
						de.desc_is_system = true;
					}
				}
			}
		}

		// --- enregistrement ---------------------------------------------------
		for(const char* reg : { "c:RegisterEffect", "Duel.RegisterEffect" }) {
			size_t rp = FindCall(line, reg);
			if(rp == std::string::npos)
				continue;
			std::string args;
			if(!ParenArgs(line, rp, args))
				continue;
			std::vector<std::string> a = SplitArgs(args);
			if(a.empty())
				continue;
			auto it = binding.find(a[0]);
			if(it != binding.end())
				effects[it->second].registered = true;
		}
		// `e:GetHandler():RegisterEffect(e1)` et variantes : on rattrape par le
		// suffixe, sans quoi des etats accordes disparaitraient de la table.
		{
			size_t rp = line.find(":RegisterEffect(");
			if(rp != std::string::npos) {
				std::string args;
				if(ParenArgs(line, rp, args)) {
					std::vector<std::string> a = SplitArgs(args);
					if(!a.empty()) {
						auto it = binding.find(a[0]);
						if(it != binding.end())
							effects[it->second].registered = true;
					}
				}
			}
		}

		// --- ce que l'operateur PRODUIT --------------------------------------
		for(const char* oi : { "Duel.SetOperationInfo",
							   "Duel.SetPossibleOperationInfo" }) {
			size_t op = FindCall(line, oi);
			if(op == std::string::npos)
				continue;
			std::string args;
			if(!ParenArgs(line, op, args))
				continue;
			std::vector<std::string> a = SplitArgs(args);
			if(a.size() < 2)
				continue;
			DeclaredProduct pr;
			pr.category_name = a[1];
			pr.category = eval_or_zero(a[1]);
			if(a.size() >= 6) {
				pr.location_name = a[5];
				pr.location = eval_or_zero(a[5]);
			}
			pr.in_function = cur_fn;
			pr.possible = std::strstr(oi, "Possible") != nullptr;
			co.products.push_back(pr);
		}

		// --- chaines declarees (l'identite des prompts) -----------------------
		{
			size_t from = 0;
			while((from = FindCall(line, "aux.Stringid", from)) !=
				  std::string::npos) {
				std::string sargs;
				size_t endp = 0;
				if(!ParenArgs(line, from, sargs, &endp))
					break;
				std::vector<std::string> sa = SplitArgs(sargs);
				if(sa.size() >= 2) {
					uint64_t cid = 0, n = 0;
					if(sa[0] == "id")
						cid = code;
					else if(!kt.Eval(sa[0], cid))
						cid = 0;
					kt.Eval(sa[1], n);
					if(cid) {
						DeclaredString ds;
						ds.card = static_cast<uint32_t>(cid);
						ds.index = static_cast<uint32_t>(n);
						ds.value = kt.StringId(static_cast<uint32_t>(cid), static_cast<uint32_t>(n));
						ds.in_function = cur_fn;
						ds.line = lineno;
						// Le SITE, c'est-a-dire ce que le prompt SERA. On le lit
						// dans le texte qui precede l'appel : `SetDescription`,
						// `Duel.SelectYesNo`, `Duel.SelectOption`, un hint...
						const std::string head = line.substr(0, from);
						static const char* kSites[] = {
							"SetDescription", "Duel.SelectYesNo",
							"Duel.SelectEffectYesNo", "Duel.SelectOption",
							"aux.RegisterClientHint", "Duel.Hint",
							"Duel.HintMessage", "Duel.SelectSequence" };
						for(const char* s : kSites)
							if(head.find(s) != std::string::npos) {
								ds.site = s;
								break;
							}
						if(ds.site.empty())
							ds.site = "?";
						if(ds.site == "SetDescription") {
							size_t vp = head.rfind(":SetDescription");
							if(vp != std::string::npos) {
								size_t vs = vp;
								while(vs > 0 && IsIdentChar(head[vs - 1]))
									--vs;
								ds.effect_var = head.substr(vs, vp - vs);
							}
						}
						co.strings.push_back(ds);
					}
				}
				from = endp + 1;
			}
		}

		// --- procedures d'invocation (LES RECETTES, en forme machine) --------
		//
		// TOUTE procedure `<Famille>.AddProc*` est relevee, et pas une liste
		// close : c'est par la que passent aussi bien la recette d'une Fusion
		// que la POSE D'UNE ECHELLE PENDULE ou l'activation d'une
		// Polymerisation, dont l'operateur ne vit pas dans le script de la
		// carte mais dans `proc_*.lua`.
		//
		// TOUT APPEL `<Famille>.<Methode>(` DANS `initial_effect` EST UNE
		// PROCEDURE, et pas seulement `AddProc*` : Polymerisation ne declare
		// rien d'autre que `Fusion.RegisterSummonEff(c)`, et son operateur
		// d'activation vit deux niveaux plus loin. Les familles exclues sont
		// celles de l'API elle-meme (`Effect`, `Duel`, `Card`, `Group`) — s'en
		// tenir a une liste de METHODES autorisees serait, lui, un catalogue.
		if(cur_fn == init_fn) {
			size_t cp = 0;
			while((cp = line.find('.', cp)) != std::string::npos) {
				size_t ns = cp;
				while(ns > 0 && IsIdentChar(line[ns - 1]))
					--ns;
				size_t me = cp + 1;
				while(me < line.size() && IsIdentChar(line[me]))
					++me;
				cp = me;
				if(ns == cp || !std::isupper(static_cast<unsigned char>(line[ns])))
					continue;
				if(me >= line.size() || line[me] != '(')
					continue;
				const std::string full = line.substr(ns, me - ns);
				const std::string fam = full.substr(0, full.find('.'));
				if(fam == "Effect" || fam == "Duel" || fam == "Card" ||
				   fam == "Group" || fam == "Auxiliary" || fam == "Debug")
					continue;
				if(std::find(co.proc_calls.begin(), co.proc_calls.end(), full) ==
				   co.proc_calls.end())
					co.proc_calls.push_back(full);
			}
		}
		if(cur_fn == init_fn) {
			size_t pp = line.find(".AddProc");
			if(pp != std::string::npos) {
				size_t ns = pp;
				while(ns > 0 && IsIdentChar(line[ns - 1]))
					--ns;
				size_t pe = pp + 1;
				while(pe < line.size() && IsIdentChar(line[pe]))
					++pe;
				const std::string pn = line.substr(ns, pe - ns);
				std::string args;
				if(ns < pp && ParenArgs(line, pe, args)) {
			DeclaredRecipe rc;
			rc.proc = pn;
			rc.line = lineno;
			std::vector<std::string> a = SplitArgs(args);
			// `Fusion.AddProcMixN(c, sub, insub, m1, n1, m2, n2, ...)` : les
			// materiaux vont par PAIRES (filtre, compte). Un filtre litteral
			// numerique est un CODE de carte ; `IsSetCard(SET_x)` est un
			// archetype ; tout le reste est un cardinal non nomme.
			//
			// Les autres procedures ne portent pas de paires : on n'en tire
			// AUCUNE exigence plutot qu'une exigence inventee.
			size_t first = a.size();
			if(pn == "Fusion.AddProcMixN")
				first = 3;
			else if(pn == "Fusion.AddProcMix" || pn == "Fusion.AddProcMixRep")
				first = 3;
			for(size_t i = first; i + 1 < a.size(); i += 2) {
				const std::string& f = a[i];
				uint64_t cnt = 1;
				kt.Eval(a[i + 1], cnt);
				bool numeric = !f.empty() &&
							   std::isdigit(static_cast<unsigned char>(f[0]));
				if(numeric) {
					rc.named.emplace_back(
						static_cast<uint32_t>(std::strtoul(f.c_str(), nullptr, 10)),
						static_cast<uint32_t>(cnt));
					continue;
				}
				size_t sc = f.find("SET_");
				if(sc != std::string::npos) {
					size_t e = sc;
					while(e < f.size() && IsIdentChar(f[e]))
						++e;
					uint64_t v = 0;
					if(kt.Lookup(f.substr(sc, e - sc), v)) {
						rc.setcode.emplace_back(v, static_cast<uint32_t>(cnt));
						continue;
					}
				}
				rc.unresolved_counts.push_back(static_cast<uint32_t>(cnt));
			}
					co.recipes.push_back(rc);
				}
			}
		}
		if(line.find("AddMustBeFusionSummoned") != std::string::npos &&
		   !co.recipes.empty())
			co.recipes.back().must_be_fusion_summoned = true;

		// --- listes declarees -------------------------------------------------
		auto read_list = [&](const char* key, std::vector<uint64_t>& out) {
			size_t kp = line.find(key);
			if(kp == std::string::npos)
				return;
			size_t ob = line.find('{', kp);
			size_t cb = line.find('}', ob == std::string::npos ? kp : ob);
			if(ob == std::string::npos || cb == std::string::npos)
				return;
			for(const std::string& t : SplitArgs(line.substr(ob + 1, cb - ob - 1))) {
				uint64_t v = 0;
				if(kt.Eval(t, v))
					out.push_back(v);
			}
		};
		{
			std::vector<uint64_t> tmp;
			read_list("s.listed_names", tmp);
			for(uint64_t v : tmp)
				co.listed_names.push_back(static_cast<uint32_t>(v));
			tmp.clear();
			read_list("s.listed_series", tmp);
			for(uint64_t v : tmp)
				co.listed_series.push_back(v);
		}

		// --- zones et verbes touches par la fonction courante -----------------
		if(cur_fn != "<chunk>" && cur_fn != init_fn) {
			size_t lp = 0;
			while((lp = line.find("LOCATION_", lp)) != std::string::npos) {
				size_t e = lp;
				while(e < line.size() && IsIdentChar(line[e]))
					++e;
				uint64_t v = 0;
				if(kt.Lookup(line.substr(lp, e - lp), v))
					co.fn_locations[cur_fn] |= v;
				lp = e;
			}
			size_t dp = 0;
			while((dp = line.find("Duel.", dp)) != std::string::npos) {
				size_t e = dp + 5;
				while(e < line.size() && IsIdentChar(line[e]))
					++e;
				const std::string verb = line.substr(dp, e - dp);
				auto& v = co.fn_verbs[cur_fn];
				if(std::find(v.begin(), v.end(), verb) == v.end())
					v.push_back(verb);
				dp = e;
			}
		}
	}

	// EN MODE PROCEDURE, un effet cree dans la fonction cible compte MEME s'il
	// n'y est pas enregistre : `Fusion.CreateSummonEff` le CONSTRUIT et le
	// RETOURNE, c'est `Fusion.RegisterSummonEff` qui l'enregistre. Exiger
	// l'enregistrement sur place perdrait exactement les operateurs qu'on est
	// venu chercher.
	const bool proc_mode = init_fn != "initial_effect";
	for(DeclaredEffect& de : effects) {
		if(de.at_init) {
			if(de.registered || proc_mode)
				co.operators.push_back(de);
		} else if(de.registered) {
			co.grants.push_back(de);
		}
	}
	return co;
}

// Les fonctions que `fn` appelle dans son propre corps, de la forme `NS.FN(`.
// UN SEUL NIVEAU est suivi, et c'est deliberé : `Fusion.RegisterSummonEff`
// delegue a `Fusion.CreateSummonEff`, et deux niveaux suffisent a couvrir
// toutes les procedures du jeu. Suivre plus loin transformerait une lecture en
// interpretation.
std::vector<std::string> CalleesOf(const std::vector<char>& src,
								   const std::string& fn) {
	std::vector<std::string> out;
	const std::string all(src.begin(), src.end());
	size_t at = all.find("function " + fn + "(");
	if(at == std::string::npos)
		at = all.find(fn + " = ");
	if(at == std::string::npos)
		at = all.find(fn + "=");
	if(at == std::string::npos)
		return out;
	// Fin du bloc : la prochaine declaration de premier niveau.
	size_t stop = all.find("\nfunction ", at + 1);
	size_t stop2 = all.find("\n" + fn.substr(0, fn.find('.')) + ".", at + 1);
	if(stop2 != std::string::npos && (stop == std::string::npos || stop2 < stop))
		stop = stop2;
	const std::string body = all.substr(at, stop == std::string::npos
											   ? std::string::npos : stop - at);
	size_t cp = 0;
	while((cp = body.find('.', cp)) != std::string::npos) {
		size_t ns = cp;
		while(ns > 0 && IsIdentChar(body[ns - 1]))
			--ns;
		size_t me = cp + 1;
		while(me < body.size() && IsIdentChar(body[me]))
			++me;
		const size_t prev = cp;
		cp = me;
		if(ns == prev || me >= body.size() || body[me] != '(')
			continue;
		if(!std::isupper(static_cast<unsigned char>(body[ns])))
			continue;
		const std::string full = body.substr(ns, me - ns);
		if(full == fn)
			continue;
		const std::string fam = full.substr(0, full.find('.'));
		if(fam == "Effect" || fam == "Duel" || fam == "Card" || fam == "Group")
			continue;
		if(std::find(out.begin(), out.end(), full) == out.end())
			out.push_back(full);
	}
	return out;
}

// --- LA TABLE ----------------------------------------------------------------

uint64_t OperatorTable::ActivatableMask(const ConstantTable& kt) {
	uint64_t m = 0, v = 0;
	for(const char* n : { "EFFECT_TYPE_ACTIVATE", "EFFECT_TYPE_IGNITION",
						  "EFFECT_TYPE_FLIP", "EFFECT_TYPE_TRIGGER_O",
						  "EFFECT_TYPE_QUICK_O", "EFFECT_TYPE_TRIGGER_F",
						  "EFFECT_TYPE_QUICK_F" })
		if(kt.Lookup(n, v))
			m |= v;
	return m;
}

size_t OperatorTable::Build(const CardDB& db, ScriptProvider& sp,
							const ConstantTable& kt,
							const std::vector<uint32_t>& codes) {
	std::vector<uint32_t> uniq;
	for(uint32_t c : codes)
		uniq.push_back(db.Canonical(c));
	std::sort(uniq.begin(), uniq.end());
	uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());

	// Les fichiers de PROCEDURE du jeu. Ce sont des scripts comme les autres, et
	// c'est la seule raison pour laquelle cette liste est acceptable : elle ne
	// nomme aucune carte, seulement des fichiers du moteur de regles. Un fichier
	// absent est saute — le harnais le dira en comptant les operateurs
	// introuvables, jamais en devinant.
	static const char* kProcFiles[] = {
		"proc_fusion.lua", "proc_fusion_spell.lua", "proc_link.lua",
		"proc_maximum.lua", "proc_normal.lua", "proc_pendulum.lua",
		"proc_ritual.lua", "proc_synchro.lua", "proc_union.lua",
		"proc_xyz.lua", "proc_workaround.lua" };
	std::vector<std::pair<std::string, std::vector<char>>> proc_src;
	for(const char* f : kProcFiles) {
		std::vector<char> s = sp.Read(f);
		if(!s.empty())
			proc_src.emplace_back(f, std::move(s));
	}

	size_t read = 0;
	for(uint32_t c : uniq) {
		char name[32];
		std::snprintf(name, sizeof name, "c%u.lua", c);
		std::vector<char> src = sp.Read(name);
		CardOperators co = ParseScript(c, src, kt);
		co.script = name;
		if(!co.script_found)
			++missing;
		else
			++read;
		// LES OPERATEURS DECLARES PAR UNE PROCEDURE. Une carte qui appelle
		// `Pendulum.AddProcedure` a une activation « poser l'echelle depuis la
		// MAIN » qui n'est ecrite nulle part dans son script : elle est dans
		// `proc_pendulum.lua`. Polymerisation ne declare RIEN d'autre que
		// `Fusion.RegisterSummonEff(c)`. Sans cette lecture, le harnais compte
		// en echec des activations parfaitement declarees — ailleurs.
		for(const std::string& pcall : co.proc_calls) {
			bool got = false;
			for(const auto& [fname, fsrc] : proc_src) {
				CardOperators pc = ParseScript(c, fsrc, kt, pcall);
				std::string via = pcall;
				if(pc.operators.empty()) {
					// UN niveau d'indirection : `RegisterSummonEff` delegue a
					// `CreateSummonEff`, qui est celle qui cree l'effet.
					for(const std::string& callee : CalleesOf(fsrc, pcall)) {
						CardOperators deep = ParseScript(c, fsrc, kt, callee);
						if(deep.operators.empty())
							continue;
						pc = std::move(deep);
						via = pcall + " -> " + callee;
						break;
					}
				}
				if(pc.operators.empty())
					continue;
				for(DeclaredEffect& e : pc.operators) {
					e.from_proc = via;
					co.operators.push_back(e);
				}
				got = true;
				break;   // la procedure est declaree dans UN fichier
			}
			(void)got;
		}
		cards[c] = std::move(co);
	}
	// Index des descriptions. Une collision (deux cartes qui declarent la meme
	// valeur) est possible en theorie et sans consequence ici : la valeur PORTE
	// le code de la carte, donc deux entrees identiques designent la meme carte.
	for(auto& [c, co] : cards)
		for(size_t i = 0; i < co.strings.size(); ++i)
			by_desc.emplace(co.strings[i].value, std::make_pair(c, i));
	return read;
}

const CardOperators* OperatorTable::Find(uint32_t code) const {
	auto it = cards.find(code);
	return it == cards.end() ? nullptr : &it->second;
}

const DeclaredString* OperatorTable::ByDesc(uint64_t desc) const {
	auto it = by_desc.find(desc);
	if(it == by_desc.end())
		return nullptr;
	const CardOperators* co = Find(it->second.first);
	if(!co || it->second.second >= co->strings.size())
		return nullptr;
	return &co->strings[it->second.second];
}

const DeclaredEffect* OperatorTable::EffectOf(const DeclaredString& s) const {
	if(s.site != "SetDescription")
		return nullptr;
	const CardOperators* co = Find(s.card);
	if(!co)
		return nullptr;
	for(const DeclaredEffect& e : co->operators)
		if(e.desc_value == s.value)
			return &e;
	// Un effet dont la description est posee par un clone : on retombe sur le
	// nom de variable, qui est la seule autre attache disponible.
	if(!s.effect_var.empty())
		for(const DeclaredEffect& e : co->operators)
			if(e.var == s.effect_var)
				return &e;
	return nullptr;
}

const DeclaredEffect* OperatorTable::SoleUndescribed(uint32_t code,
													 uint64_t act_mask) const {
	const CardOperators* co = Find(code);
	if(!co)
		return nullptr;
	const DeclaredEffect* found = nullptr;
	for(const DeclaredEffect& e : co->operators) {
		if(!(e.etype & act_mask) || e.has_desc)
			continue;
		if(found)
			return nullptr;   // ambigu : on ne devine pas
		found = &e;
	}
	return found;
}

size_t OperatorTable::OperatorCount() const {
	size_t n = 0;
	for(const auto& [c, co] : cards)
		n += co.operators.size();
	return n;
}
size_t OperatorTable::GrantCount() const {
	size_t n = 0;
	for(const auto& [c, co] : cards)
		n += co.grants.size();
	return n;
}
size_t OperatorTable::ProductCount() const {
	size_t n = 0;
	for(const auto& [c, co] : cards)
		n += co.products.size();
	return n;
}

void OperatorTable::Print(const CardDB& db, const ConstantTable& kt) const {
	const uint64_t act = ActivatableMask(kt);
	std::vector<uint32_t> ordered;
	for(const auto& [c, co] : cards)
		ordered.push_back(c);
	std::sort(ordered.begin(), ordered.end());

	std::printf("\n=== TABLE D'OPERATEURS DECLARES (analyse statique du Lua) ===\n");
	std::printf("  %zu carte(s) lue(s), %zu sans script, %zu operateur(s), "
				"%zu etat(s) accorde(s), %zu declaration(s) de produit\n",
				cards.size() - missing, missing, OperatorCount(), GrantCount(),
				ProductCount());
	std::printf("  constantes : %zu, depuis %zu fichier(s) du jeu\n",
				kt.Size(), kt.Files().size());
	std::printf("  RAPPEL : cette table est DECLARATIVE et OPTIMISTE — conditions "
				"et couts sont des\n"
				"  fermetures, non evaluees ici. Elle dit ce qu'une carte declare "
				"pouvoir faire.\n");

	for(uint32_t c : ordered) {
		const CardOperators& co = cards.at(c);
		if(!co.script_found) {
			std::printf("\n  %-8u %-34s   (aucun script : %s)\n", c,
						db.Name(c).c_str(), co.script.c_str());
			continue;
		}
		bool interesting = !co.operators.empty() || !co.grants.empty() ||
						   !co.recipes.empty();
		if(!interesting)
			continue;
		std::printf("\n  %-8u %s\n", c, db.Name(c).c_str());
		for(const DeclaredRecipe& rc : co.recipes) {
			std::printf("      RECETTE  %s%s\n", rc.proc.c_str(),
						rc.must_be_fusion_summoned
							? "  + AddMustBeFusionSummoned" : "");
			for(const auto& [mc, n] : rc.named)
				std::printf("               materiau NOMME %u x%u  (%s)\n", mc, n,
							db.Name(mc).c_str());
			for(const auto& [sc, n] : rc.setcode)
				std::printf("               archetype 0x%llx x%u\n",
							(unsigned long long)sc, n);
			for(uint32_t n : rc.unresolved_counts)
				std::printf("               cardinal non resolu x%u\n", n);
		}
		for(const DeclaredEffect& e : co.operators) {
			const bool usable = (e.etype & act) != 0;
			std::printf("      %s %-3s type=%s\n", usable ? "OPERATEUR" : "passif  ",
						e.var.c_str(),
						kt.MaskNames("EFFECT_TYPE_", e.etype).c_str());
			if(e.range)
				std::printf("               zone exigee : %s%s\n",
							kt.MaskNames("LOCATION_", e.range).c_str(),
							NormalizeRange(e.range) != e.range
								? "   (zone SYMBOLIQUE : le message porte la zone "
								  "physique)" : "");
			if(e.code_is_event)
				std::printf("               declenche sur : %s\n", e.code_name.c_str());
			if(e.code_is_effect)
				std::printf("               ACCORDE : %s\n", e.code_name.c_str());
			if(e.category)
				std::printf("               categorie : %s\n",
							kt.MaskNames("CATEGORY_", e.category).c_str());
			if(e.has_count_limit)
				std::printf("               ressource : %u/tour %s\n",
							e.count_limit,
							e.count_by_name ? "par NOM" : "par COPIE");
			if(e.desc_value)
				std::printf("               description : %llu  (id %u, chaine %u)\n",
							(unsigned long long)e.desc_value, e.desc_card,
							e.desc_index);
			std::string fns;
			if(!e.fn_cost.empty())      fns += "cout=" + e.fn_cost + " ";
			if(!e.fn_condition.empty()) fns += "cond=" + e.fn_condition + " ";
			if(!e.fn_target.empty())    fns += "cible=" + e.fn_target + " ";
			if(!e.fn_operation.empty()) fns += "op=" + e.fn_operation;
			if(!fns.empty())
				std::printf("               %s\n", fns.c_str());
			// La CONSOMMATION declaree : les zones que le cout touche.
			for(const std::string& fn : { e.fn_cost, e.fn_operation, e.fn_target }) {
				if(fn.empty())
					continue;
				std::string base = fn;
				if(base.compare(0, 2, "s.") == 0)
					base = base.substr(2);
				auto it = co.fn_locations.find(base);
				if(it != co.fn_locations.end() && it->second)
					std::printf("               %s touche : %s\n", base.c_str(),
								kt.MaskNames("LOCATION_", it->second).c_str());
			}
		}
		for(const DeclaredEffect& e : co.grants) {
			std::printf("      ACCORDE  %s   (pose par %s)\n",
						e.code_name.empty() ? "<sans code>" : e.code_name.c_str(),
						e.in_function.c_str());
			if(e.target_range_self || e.target_range_opp)
				std::printf("               portee : %s / %s\n",
							kt.MaskNames("LOCATION_", e.target_range_self).c_str(),
							kt.MaskNames("LOCATION_", e.target_range_opp).c_str());
			if(e.has_count_limit)
				std::printf("               ressource : %u %s\n", e.count_limit,
							e.count_by_name ? "par NOM" : "par COPIE");
		}
		for(const DeclaredProduct& p : co.products)
			std::printf("      PRODUIT  %s @ %s   (%s%s)\n",
						p.category_name.c_str(),
						p.location_name.empty() ? "-" : p.location_name.c_str(),
						p.in_function.c_str(), p.possible ? ", eventuel" : "");
	}
}

void OperatorTable::PrintGrants(const CardDB& db, const ConstantTable& kt) const {
	(void)kt;
	std::printf("\n--- LES ETATS ACCORDES (le vocabulaire EFFECT_*, "
				"celui que CATEGORY_* ne voit pas) ---\n");
	std::vector<std::pair<std::string, std::string>> rows;
	for(const auto& [c, co] : cards) {
		for(const DeclaredEffect& e : co.grants) {
			if(e.code_name.empty() || !e.code_is_effect)
				continue;
			char buf[256];
			std::snprintf(buf, sizeof buf, "  %-36s -> %s", db.Name(c).c_str(),
						  e.code_name.c_str());
			rows.emplace_back(e.code_name + db.Name(c), buf);
		}
	}
	std::sort(rows.begin(), rows.end());
	for(const auto& [k, s] : rows)
		std::printf("%s\n", s.c_str());
	if(rows.empty())
		std::printf("  (aucun)\n");
	std::printf("  %zu etat(s) accorde(s) en resolution sur %zu carte(s). "
				"C'est ICI que vivent les aretes\n"
				"  que le graphe de recettes n'a jamais eues.\n",
				rows.size(), cards.size());
}

// --- LE TYPE DE NŒUD MANQUANT ------------------------------------------------

std::vector<AcquirableCode> AcquirableCodesOf(const OperatorTable& tbl,
											  const CardDB& db,
											  const std::vector<uint32_t>& owned) {
	std::vector<AcquirableCode> out;
	for(const auto& [c, co] : tbl.All()) {
		for(const DeclaredEffect& g : co.grants) {
			if(g.code_name != "EFFECT_ADD_CODE" &&
			   g.code_name != "EFFECT_CHANGE_CODE")
				continue;
			// L'OPERATEUR QUI POSE CET ETAT. Le lien est le nom de fonction :
			// l'etat est cree dans `s.operation`, et l'operateur declare
			// `SetOperation(s.operation)`. Aucune autre attache n'existe, et
			// c'est celle que les scripts ecrivent.
			const DeclaredEffect* host = nullptr;
			for(const DeclaredEffect& e : co.operators) {
				for(const std::string* fn : { &e.fn_operation, &e.fn_cost,
											  &e.fn_target }) {
					std::string base = *fn;
					if(base.compare(0, 2, "s.") == 0)
						base = base.substr(2);
					if(!base.empty() && base == g.in_function) {
						host = &e;
						break;
					}
				}
				if(host)
					break;
			}
			if(!host)
				continue;
			// LES ZONES QUE L'OPERATEUR ATTEINT, lues dans ses fonctions. Pour
			// `Kaleido Chick`, le cout balaye `LOCATION_DECK|LOCATION_EXTRA` —
			// et c'est de la que le code vient.
			uint64_t zones = 0;
			for(const std::string* fn : { &host->fn_cost, &host->fn_operation,
										  &host->fn_target }) {
				std::string base = *fn;
				if(base.compare(0, 2, "s.") == 0)
					base = base.substr(2);
				auto it = co.fn_locations.find(base);
				if(it != co.fn_locations.end())
					zones |= it->second;
			}
			if(!zones)
				continue;
			// LES CODES ATTEIGNABLES. `s.listed_series` est la DECLARATION que
			// la carte fait de l'archetype qu'elle manipule : s'en servir n'est
			// pas du reglage, c'est lire ce que le script annonce au moteur.
			// Sans declaration, on ne restreint pas — sous-estimer est la
			// direction sure (regle 2), inventer un filtre ne l'est pas.
			for(uint32_t oc : owned) {
				const uint32_t code = db.Canonical(oc);
				const CardRow* row = db.Find(code);
				if(!row || !(row->type & 0x1u))   // TYPE_MONSTER
					continue;
				if(!co.listed_series.empty()) {
					bool hit = false;
					for(uint64_t want : co.listed_series) {
						for(uint16_t sc : row->setcodes) {
							if(!sc)
								continue;
							const uint16_t w = static_cast<uint16_t>(want);
							if((sc & 0x0fffu) == (w & 0x0fffu) &&
							   (sc & w & 0xf000u) == (w & 0xf000u)) {
								hit = true;
								break;
							}
						}
						if(hit)
							break;
					}
					if(!hit)
						continue;
				}
				// ACQUERIR SON PROPRE NOM NE PRODUIT RIEN. L'arete serait une
				// boucle sur elle-meme dans un graphe deja cyclique, et la
				// distance la paierait a chaque niveau de recursion.
				if(code == c)
					continue;
				AcquirableCode ac;
				ac.code = code;
				ac.host = c;
				ac.host_range = NormalizeRange(host->range);
				ac.source_zone = zones;
				ac.grant = g.code_name;
				out.push_back(ac);
			}
		}
	}
	std::sort(out.begin(), out.end(), [](const AcquirableCode& a,
										 const AcquirableCode& b) {
		if(a.code != b.code) return a.code < b.code;
		return a.host < b.host;
	});
	out.erase(std::unique(out.begin(), out.end(),
						  [](const AcquirableCode& a, const AcquirableCode& b) {
							  return a.code == b.code && a.host == b.host;
						  }), out.end());
	return out;
}

// --- LE HARNAIS --------------------------------------------------------------

HarnessVerdict ConfrontPlan(const OperatorTable& tbl, const CardDB& db,
							const ConstantTable& kt,
							const std::vector<ObservedActivation>& acts) {
	HarnessVerdict v;
	const uint64_t act_mask = OperatorTable::ActivatableMask(kt);
	// Compteur de ressource : (tour, carte, index de chaine) -> emplois.
	std::map<std::tuple<int, uint32_t, uint32_t>, uint32_t> used_by_name;

	for(const ObservedActivation& a : acts) {
		++v.total;
		const CardOperators* co = a.code ? tbl.Find(a.code) : nullptr;
		if(a.code && (!co || !co->script_found)) {
			++v.no_script;
			continue;
		}
		const DeclaredString* ds = a.desc ? tbl.ByDesc(a.desc) : nullptr;
		const DeclaredEffect* de = nullptr;
		bool zone_is_evidence = false;   // la zone a servi a DESIGNER l'operateur
		bool exact = true;               // l'operateur est-il DETERMINE ?
		if(ds) {
			++v.matched_by_desc;
			de = tbl.EffectOf(*ds);
		} else if(co) {
			// La description n'est pas un `aux.Stringid` d'une carte du deck.
			// Deux cas, et ils ne disent pas la meme chose.
			//
			// (1) L'OPERATEUR DECLARE LUI-MEME CETTE CHAINE SYSTEME. C'est le
			//     cas des procedures : `Pendulum.AddProcedure` pose
			//     `SetDescription(1160)` (« activer comme Magie Pendule »).
			//     L'appariement reste EXACT.
			for(const DeclaredEffect& e : co->operators)
				if(e.desc_is_system && e.desc_value == a.desc) {
					de = &e;
					break;
				}
			if(de) {
				++v.matched_by_system;
			} else {
				// (2) LA DESCRIPTION VIENT DU CORE. `processor.cpp:743` emet 221
				//     et `:443` emet 0 pour « activer l'effet declencheur de
				//     cette carte ? » : le message porte la CARTE et pas
				//     l'effet. Aucune extraction ne peut faire mieux — c'est le
				//     protocole qui ne transporte pas l'information. On apparie
				//     donc a la carte, en departageant par la ZONE quand elle
				//     suffit, et l'on COMPTE ce qui reste ambigu.
				std::vector<const DeclaredEffect*> cand;
				for(const DeclaredEffect& e : co->operators)
					if(e.etype & act_mask)
						cand.push_back(&e);
				if(cand.size() > 1 && a.location) {
					std::vector<const DeclaredEffect*> keep;
					for(const DeclaredEffect* e : cand)
						if(!e->range || (NormalizeRange(e->range) & a.location))
							keep.push_back(e);
					if(!keep.empty() && keep.size() < cand.size()) {
						cand.swap(keep);
						zone_is_evidence = true;
					}
				}
				if(cand.empty()) {
					++v.unmatched;
					if(v.failures.size() < 40) {
						char buf[256];
						std::snprintf(buf, sizeof buf,
									  "  #%-4zu %-30s desc=%llu : AUCUN "
									  "operateur declare", a.at,
									  db.Name(a.code).c_str(),
									  (unsigned long long)a.desc);
						v.failures.emplace_back(buf);
					}
					continue;
				}
				++v.matched_by_card;
				if(cand.size() > 1) {
					++v.ambiguous;
					// L'OPERATEUR N'EST PAS DETERMINE. Juger une precondition
					// sur un operateur choisi arbitrairement rendrait des
					// ECARTS FABRIQUES : `Lunalight Gold Leo` a trois effets
					// declencheurs, dont deux partagent un compteur et le
					// troisieme en a un autre (`SetCountLimit(1,{id,1})`).
					// Prendre le premier et compter ses emplois fait sonner une
					// « ressource depassee » qui n'existe pas.
					exact = false;
				}
				de = cand.front();
			}
		}
		if(!de)
			continue;   // apparie a une chaine de RESOLUTION : pas de zone a juger
		if(!exact)
			continue;   // rien de verifiable : l'operateur n'est pas designe
		// Une zone qui a servi a DESIGNER l'operateur ne peut pas ensuite le
		// juger : le controle serait vide par construction (piege 42).
		// PRECONDITION DE ZONE. Elle n'est verifiable que sur les prompts qui
		// portent la zone d'activation (IDLECMD, CHAIN) et pour un effet qui
		// declare une portee.
		if(!zone_is_evidence && de->range && a.location) {
			++v.zone_checked;
			const uint64_t want = NormalizeRange(de->range);
			if(want & a.location) {
				++v.zone_ok;
			} else {
				++v.zone_violated;
				if(v.failures.size() < 40) {
					char buf[320];
					std::snprintf(buf, sizeof buf,
								  "  #%-4zu %-30s zone %s attendue %s : ECART",
								  a.at, db.Name(a.code).c_str(),
								  kt.MaskNames("LOCATION_", a.location).c_str(),
								  kt.MaskNames("LOCATION_", de->range).c_str());
					v.failures.emplace_back(buf);
				}
			}
		}
		// PRECONDITION DE RESSOURCE. `SetCountLimit(n, id)` compte par JOUEUR et
		// par (code, hopt_index) — verifie contre `field::get_count_map`
		// (`field.cpp:1452`, cle `code << 32 | hopt_index << 16 | flag << 8 |
		// playerid`) et non suppose : c'est donc bien « n fois par tour et par
		// NOM », et deux effets d'une meme carte se partagent le compteur SAUF
		// s'ils portent des index differents (`SetCountLimit(1,{id,1})`).
		// La cle est donc le TAG ecrit dans le script, jamais l'index de
		// description — deux effets peuvent partager la description et pas le
		// compteur, et l'inverse.
		//
		// La variante SANS tag (`SetCountLimit(1)`) est « par copie » : elle
		// exigerait de suivre l'identite physique d'une carte a travers ses
		// deplacements, et n'est donc pas verifiable depuis les messages.
		if(de->has_count_limit && de->count_by_name && de->count_limit) {
			++v.count_checked;
			auto key = std::make_tuple(a.turn, a.code,
									   static_cast<uint32_t>(
										   std::hash<std::string>{}(de->count_tag)));
			if(++used_by_name[key] > de->count_limit) {
				++v.count_violated;
				if(v.failures.size() < 40) {
					char buf[256];
					std::snprintf(buf, sizeof buf,
								  "  #%-4zu %-30s ressource %u/tour depassee",
								  a.at, db.Name(a.code).c_str(), de->count_limit);
					v.failures.emplace_back(buf);
				}
			}
		}
	}
	return v;
}

void PrintVerdict(const HarnessVerdict& v) {
	std::printf("\n=== HARNAIS : LA TABLE EXPLIQUE-T-ELLE LE PLAN ? ===\n");
	std::printf("  activations relevees        : %zu\n", v.total);
	std::printf("  appariees par DESCRIPTION   : %zu   (aux.Stringid : un "
				"operateur et un seul)\n", v.matched_by_desc);
	std::printf("  appariees par chaine SYSTEME: %zu   (l'operateur declare "
				"cette chaine : exact)\n", v.matched_by_system);
	std::printf("  appariees par CARTE         : %zu   (description fabriquee "
				"par le core ;\n"
				"                                       le message ne dit pas "
				"QUEL effet), dont %zu ambigue(s)\n",
				v.matched_by_card, v.ambiguous);
	std::printf("  carte hors table            : %zu\n", v.no_script);
	std::printf("  NON APPARIEES               : %zu%s\n", v.unmatched,
				v.unmatched ? "   <-- LA TABLE NE DECRIT PAS LE JEU" : "");
	std::printf("  precondition de ZONE        : %zu verifiee(s), %zu tenue(s), "
				"%zu ecart(s)%s\n", v.zone_checked, v.zone_ok, v.zone_violated,
				v.zone_violated ? "   <-- ECART" : "");
	std::printf("  precondition de RESSOURCE   : %zu verifiee(s), %zu "
				"depassement(s)%s\n", v.count_checked, v.count_violated,
				v.count_violated ? "   <-- ECART" : "");
	if(!v.failures.empty()) {
		std::printf("  detail (borne a 40) :\n");
		for(const std::string& f : v.failures)
			std::printf("%s\n", f.c_str());
	}
	const bool ok = v.unmatched == 0 && v.zone_violated == 0 &&
					v.count_violated == 0;
	std::printf("  => %s\n", ok
		? "LA TABLE EXPLIQUE LE PLAN : toute activation correspond a un operateur "
		  "declare,\n     et la sequence est valide sous les preconditions "
		  "extraites."
		: "LA TABLE NE SUFFIT PAS. Le chantier suivant est SANS OBJET tant que "
		  "cet ecart\n     n'est pas explique — c'est exactement ce qu'on voulait "
		  "savoir en premier.");
}

} // namespace solver
