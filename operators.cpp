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
			// --- CE QUE LA FONCTION DESIGNE : archetype et code ---------------
			//
			// Meme mecanisme que les zones ci-dessus, et il sert les DEUX
			// colonnes qui manquaient au bilan (9.30) :
			//
			//   `IsSetCard(SET_X)` dans un filtre de cout dit QUEL archetype la
			//   destruction touche — « une Lunalight de l'EXTRA », et non « une
			//   carte quelconque ». Sans lui, la colonne negative etait typee
			//   par ZONE seulement, donc inecrivable sans l'inventer.
			//
			//   `IsCode(n)` dit quel code exact une recette ou un filtre exige.
			//
			// On ne lit pas la semantique du filtre : on releve les CONSTANTES
			// qu'il nomme. Un filtre qui nommerait deux archetypes rendrait deux
			// entrees, et c'est a l'appelant de COMPTER l'ambiguite plutot que
			// de trancher — la regle du dossier depuis le harnais.
			auto scan_call = [&](const char* fn_name, auto&& sink) {
				const size_t nlen = std::strlen(fn_name);
				size_t q = 0;
				while((q = line.find(fn_name, q)) != std::string::npos) {
					size_t b = q + nlen;
					size_t e2 = b;
					while(e2 < line.size() && line[e2] != ')' && line[e2] != ',')
						++e2;
					sink(Trim(line.substr(b, e2 - b)));
					q = e2;
				}
			};
			// LES FONCTIONS QUE CELLE-CI APPELLE. La contrainte ne vit presque
			// jamais dans la fonction qui detruit : `descost` fait
			// `SelectMatchingCard(tp, s.descostfilter, ..., LOCATION_EXTRA, ...)`
			// et c'est le FILTRE qui porte `IsSetCard(SET_LUNALIGHT)`. Sans ce
			// saut d'un niveau, toute arete negative reste « place
			// INDETERMINEE » — mesure faite, les deux du deck l'etaient.
			{
				size_t sp = 0;
				while((sp = line.find("s.", sp)) != std::string::npos) {
					const bool word_start =
						sp == 0 || !IsIdentChar(line[sp - 1]);
					size_t e2 = sp + 2;
					while(e2 < line.size() && IsIdentChar(line[e2]))
						++e2;
					if(word_start && e2 > sp + 2) {
						const std::string ref = line.substr(sp + 2, e2 - sp - 2);
						auto& v = co.fn_refs[cur_fn];
						if(ref != cur_fn &&
						   std::find(v.begin(), v.end(), ref) == v.end())
							v.push_back(ref);
					}
					sp = e2;
				}
			}
			scan_call("IsSetCard(", [&](const std::string& arg) {
				uint64_t v = 0;
				if(!arg.empty() && kt.Eval(arg, v) && v) {
					auto& s2 = co.fn_setcodes[cur_fn];
					if(std::find(s2.begin(), s2.end(), v) == s2.end())
						s2.push_back(v);
				}
			});
			scan_call("IsCode(", [&](const std::string& arg) {
				uint64_t v = 0;
				if(arg.empty())
					return;
				if(std::isdigit(static_cast<unsigned char>(arg[0])))
					v = std::strtoull(arg.c_str(), nullptr, 10);
				else
					kt.Eval(arg, v);
				if(v && v < 0xffffffffull) {
					auto& s2 = co.fn_codes[cur_fn];
					const uint32_t c32 = static_cast<uint32_t>(v);
					if(std::find(s2.begin(), s2.end(), c32) == s2.end())
						s2.push_back(c32);
				}
			});
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

namespace {

// CLASSEMENT DES VERBES, et la nuance qui decide de tout.
//
// `Duel.SendtoHand` sur un monstre d'EXTRA ne va pas a la main : le core le
// remet a l'EXTRA — c'est pourquoi les scripts testent
// `IsLocation(LOCATION_HAND|LOCATION_EXTRA)` juste apres l'appel. C'est donc un
// RECYCLEUR, une entree POSITIVE sur la place visee, et non une consommation.
// Idem `SendtoDeck`. C'est l'objection qui interdit de raisonner en STOCKS :
// le modele est un modele de FLUX.
//
// L'ASYMETRIE DU RISQUE dicte le classement, et elle n'est pas symetrique du
// tout : oublier une CONSOMMATION rend le bilan trop optimiste — plus mou,
// jamais faux. Oublier une PRODUCTION le rend NON SONORE — il tuerait une
// vraie ligne. Au moindre doute un verbe va donc dans « recycle » ou
// « autre », JAMAIS dans « detruit ». C'est la direction sure de la regle 2.
enum class VerbKind { kDestroy, kRecycle, kProduce, kOther };

VerbKind ClassifyVerb(const std::string& v) {
	static const char* kDestroy[] = {
		"Duel.SendtoGrave", "Duel.Remove", "Duel.Destroy", "Duel.Release",
		"Duel.DiscardHand", "Duel.DiscardDeck",
	};
	static const char* kRecycle[] = {
		"Duel.SendtoHand", "Duel.SendtoDeck", "Duel.ReturnToField",
		"Duel.MoveToField",
	};
	static const char* kProduce[] = {
		"Duel.SpecialSummon", "Duel.SpecialSummonStep", "Duel.Summon",
		"Duel.MSet", "Duel.SSet", "Duel.Draw",
	};
	for(const char* s : kDestroy)
		if(v == s) return VerbKind::kDestroy;
	for(const char* s : kRecycle)
		if(v == s) return VerbKind::kRecycle;
	for(const char* s : kProduce)
		if(v == s) return VerbKind::kProduce;
	return VerbKind::kOther;
}

// LES DEUX CONVENTIONS DE NOM, ET IL FAUT LES REDUIRE A UNE.
//
// `cur_fn` (cle de `fn_verbs`) est pose a la DEFINITION et retire le prefixe :
// `function s.spop(...)` -> « spop ». Les champs `fn_*` d'un effet sont poses a
// l'USAGE et le gardent : `e2:SetOperation(s.spop)` -> « s.spop ». Comparer les
// deux tels quels ne rend JAMAIS de correspondance, donc jamais de capacite —
// et un consommateur sans borne connue est exactement le cas ou le bilan ne dit
// rien. Piege 42 sous une autre forme : un rapport vivant qui imprime le
// pessimisme partout.
std::string NormFn(std::string f) {
	if(f.compare(0, 2, "s.") == 0)
		return f.substr(2);
	if(f.size() > 2 && f[0] == 'c' &&
	   std::isdigit(static_cast<unsigned char>(f[1]))) {
		const size_t d = f.find('.');
		if(d != std::string::npos)
			return f.substr(d + 1);
	}
	return f;
}

// La capacite declaree de l'operateur dont `fn` est une des fonctions. Rend nul
// quand aucun effet ne la porte, et l'appelant imprime alors « SANS BORNE » —
// lecture pessimiste pour un consommateur, optimiste pour un recycleur. On
// l'imprime plutot que de la supposer.
const DeclaredEffect* OwnerOf(const CardOperators& co, const std::string& fn) {
	for(const std::vector<DeclaredEffect>* set : {&co.operators, &co.grants})
		for(const DeclaredEffect& e : *set)
			if(NormFn(e.fn_cost) == fn || NormFn(e.fn_condition) == fn ||
			   NormFn(e.fn_target) == fn || NormFn(e.fn_operation) == fn ||
			   NormFn(e.fn_value) == fn)
				return &e;
	return nullptr;
}

}   // namespace

void OperatorTable::PrintConsumption(const CardDB& db, const ConstantTable& kt,
									 uint64_t goal_zone) const {
	std::printf("\n--- LA COLONNE NEGATIVE : ce que chaque operateur DETRUIT "
				"---\n");
	// Une ligne du bilan matiere : (carte, fonction) -> verbes classes, zones
	// touchees, capacite declaree.
	struct Edge {
		uint32_t code = 0;
		std::string fn;
		std::vector<std::string> destroy, recycle, produce;
		uint64_t locs = 0;
		uint32_t cap = 0;
		bool cap_by_name = false;
		bool has_cap = false;
	};
	std::vector<Edge> edges;
	for(const auto& [c, co] : cards) {
		for(const auto& [fn, verbs] : co.fn_verbs) {
			Edge e;
			e.code = c;
			e.fn = fn;
			for(const std::string& v : verbs) {
				switch(ClassifyVerb(v)) {
				case VerbKind::kDestroy: e.destroy.push_back(v); break;
				case VerbKind::kRecycle: e.recycle.push_back(v); break;
				case VerbKind::kProduce: e.produce.push_back(v); break;
				default: break;
				}
			}
			if(e.destroy.empty() && e.recycle.empty() && e.produce.empty())
				continue;
			auto it = co.fn_locations.find(fn);
			e.locs = it == co.fn_locations.end() ? 0 : it->second;
			if(const DeclaredEffect* owner = OwnerOf(co, fn)) {
				e.has_cap = owner->has_count_limit;
				e.cap = owner->count_limit;
				e.cap_by_name = owner->count_by_name;
			}
			edges.push_back(std::move(e));
		}
	}
	std::sort(edges.begin(), edges.end(), [&](const Edge& a, const Edge& b) {
		if(a.code != b.code) return db.Name(a.code) < db.Name(b.code);
		return a.fn < b.fn;
	});

	auto join = [](const std::vector<std::string>& v) {
		std::string s;
		for(const std::string& x : v) {
			if(!s.empty()) s += " ";
			s += x.substr(5);   // sans le prefixe "Duel."
		}
		return s;
	};
	uint32_t last = 0;
	for(const Edge& e : edges) {
		if(e.code != last) {
			std::printf("  %s\n", db.Name(e.code).c_str());
			last = e.code;
		}
		std::printf("    %-22s", e.fn.c_str());
		if(!e.destroy.empty()) std::printf("  DETRUIT %s", join(e.destroy).c_str());
		if(!e.recycle.empty()) std::printf("  RECYCLE %s", join(e.recycle).c_str());
		if(!e.produce.empty()) std::printf("  PRODUIT %s", join(e.produce).c_str());
		if(e.locs)
			std::printf("  @ %s", kt.MaskNames("LOCATION_", e.locs).c_str());
		if(e.has_cap)
			std::printf("  [cap %u/tour %s]", e.cap,
						e.cap_by_name ? "par NOM" : "par COPIE");
		std::printf("\n");
	}

	// LA SYNTHESE, ET C'EST ELLE LE JUGE. On ne garde que les aretes qui
	// consomment DANS la zone du but : c'est la ligne `A_p` de l'equation de
	// bilan, celle dont la faisabilite decide qu'un tirage est mort.
	const std::string zname = kt.MaskNames("LOCATION_", goal_zone);
	std::printf("\n  --- CE QUI CONSOMME DANS %s (la ligne du but) ---\n",
				zname.empty() ? "<zone>" : zname.c_str());
	size_t n_destroy = 0, n_recycle = 0;
	for(const Edge& e : edges) {
		if(!(e.locs & goal_zone))
			continue;
		// LA CAPACITE EST LE CHIFFRE QUI DECIDE, des deux cotes du signe : sur un
		// consommateur elle borne combien de jetons peuvent partir, sur un
		// recycleur combien peuvent revenir. « par NOM » vaut 1 quel que soit le
		// nombre de copies ; « par COPIE » vaut autant que de copies posables.
		// C'est la borne `Y_o <= cap` du cadre operator-counting, et sans elle
		// le bilan ne serre rien.
		char cap[48];
		if(e.has_cap)
			std::snprintf(cap, sizeof cap, "[cap %u/tour par %s]", e.cap,
						  e.cap_by_name ? "NOM" : "COPIE");
		else
			std::snprintf(cap, sizeof cap, "[SANS BORNE declaree]");
		if(!e.destroy.empty()) {
			++n_destroy;
			// LA PLACE, ET NON PLUS SEULEMENT LA ZONE. `IsSetCard`/`IsCode` du
			// filtre disent CE QUE la destruction touche ; sans eux l'arete
			// negative n'etait attribuable a aucune place (9.30).
			std::string what;
			auto cit = cards.find(e.code);
			if(cit != cards.end()) {
				// La fonction, PUIS les fonctions qu'elle appelle (le filtre).
				std::vector<std::string> scope{ e.fn };
				auto rit = cit->second.fn_refs.find(e.fn);
				if(rit != cit->second.fn_refs.end())
					for(const std::string& r : rit->second)
						scope.push_back(r);
				for(const std::string& f : scope) {
				auto sit = cit->second.fn_setcodes.find(f);
				if(sit != cit->second.fn_setcodes.end())
					for(uint64_t sc : sit->second) {
						char b[48];
						std::snprintf(b, sizeof b, "%sarch 0x%llx",
									  what.empty() ? "" : "|",
									  (unsigned long long)sc);
						what += b;
					}
				auto kit = cit->second.fn_codes.find(f);
				if(kit != cit->second.fn_codes.end())
					for(uint32_t cc : kit->second) {
						char b[64];
						std::snprintf(b, sizeof b, "%s%s",
									  what.empty() ? "" : "|",
									  db.Name(cc).c_str());
						what += b;
					}
				}
			}
			std::printf("  -1  %-30s %-20s %-14s %s  %s\n",
						db.Name(e.code).c_str(), e.fn.c_str(),
						join(e.destroy).c_str(), cap,
						what.empty() ? "(place INDETERMINEE)" : what.c_str());
		}
		if(!e.recycle.empty()) {
			++n_recycle;
			std::printf("  +1  %-30s %-20s %-14s %s\n", db.Name(e.code).c_str(),
						e.fn.c_str(), join(e.recycle).c_str(), cap);
		}
	}
	std::printf("\n  %zu arete(s) NEGATIVE(S), %zu RECYCLEUR(S) sur cette "
				"zone.\n", n_destroy, n_recycle);
	std::printf("  Lecture : une place dont le but exige plus de jetons qu'il "
				"n'en reste, et dont\n"
				"  AUCUNE arete positive n'est servie, rend la ligne du bilan "
				"INFAISABLE — donc\n"
				"  le tirage est mort, prouve. Un recycleur BORNE ne repousse "
				"ce mur que de sa\n"
				"  capacite : c'est la borne `Y_o <= cap`, et sans elle le "
				"bilan ne dit jamais rien.\n");
}

// --- LE TYPE DE NŒUD MANQUANT ------------------------------------------------

std::vector<AcquirableCode> AcquirableCodesOf(const OperatorTable& tbl,
											  const CardDB& db,
											  const std::vector<uint32_t>& owned) {
	std::vector<AcquirableCode> out;
	// LES SEULS CODES QUI VAILLENT D'ETRE ACQUIS : ceux qu'une recette DECLAREE
	// nomme comme MATERIAU.
	//
	// LA PREMIERE VERSION POSAIT TREIZE ARETES, ET DOUZE ETAIENT NUISIBLES.
	// « Kaleido Chick peut acquerir le code de Liger Dancer » est vrai au sens
	// du jeu — et pour l'emprunter il faut envoyer un Liger de l'EXTRA au
	// cimetiere. Le deck en a trois, le but en demande trois : l'arete propose
	// une route qui DETRUIT le but. Et elle ne sert a rien, puisque aucune
	// recette n'exige « Liger Dancer » comme materiau.
	//
	// Sur l'etalon A, une seule recette nomme quoi que ce soit (Liger exige
	// 24550676) : treize aretes deviennent UNE. C'est aussi ce qui explique la
	// distance moyenne du graphe passant de 14 a 107 — douze aretes inutiles
	// dans un graphe deja cyclique.
	//
	// CE N'EST PAS UN ELAGAGE DE L'ESPACE D'ACTIONS (regle 2 du chantier 16) :
	// rien n'est retire au solveur. On retire des aretes d'une HEURISTIQUE,
	// c'est-a-dire des routes que le graphe decrivait comme utiles et qui ne le
	// sont pas.
	//
	// LIMITE ASSUMEE : seules les recettes DECLAREES (`Fusion.AddProcMix*`)
	// comptent ici. Une exigence nommee qui ne viendrait que du TEXTE de carte
	// n'ouvrirait donc aucune acquisition — sous-estimer est la direction sure,
	// et l'amorce par le texte reste posee de son cote.
	std::vector<uint32_t> wanted;
	for(const auto& [c, co] : tbl.All())
		for(const DeclaredRecipe& rc : co.recipes)
			for(const auto& [mc, n] : rc.named)
				wanted.push_back(db.Canonical(mc));
	std::sort(wanted.begin(), wanted.end());
	wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

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
				// AUCUNE RECETTE NE DEMANDE CE CODE : l'acquisition est inutile,
				// et son cout consomme la reserve. Voir l'en-tete.
				if(!std::binary_search(wanted.begin(), wanted.end(), code))
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

// --- LE SIMPLEXE (9.30) ------------------------------------------------------
//
// Deux phases, regle de BLAND. Bland est plus lent que la regle du cout le plus
// negatif, et c'est exactement pourquoi elle est retenue : elle **garantit la
// terminaison** (aucun cyclage), et nos instances sont minuscules. Un solveur
// qui boucle sur une instance degeneree serait indetectable dans un run de
// recherche ; un solveur lent ne l'est pas.
//
// Forme resolue, apres normalisation :   min c'x   s.c.  Ex = f,  x >= 0
//   - contrainte `a.x >= b` avec b >= 0  ->  a.x - surplus = b, + artificielle
//   - contrainte `a.x >= b` avec b <  0  ->  (-a).x + ecart = -b   (base fournie)
//   - borne `x_j <= u_j`                 ->  x_j + ecart = u_j     (base fournie)
namespace {

constexpr double kEps = 1e-9;

struct Tableau {
	size_t m = 0, n = 0;                 // lignes, colonnes (hors membre droit)
	std::vector<double> a;               // m x (n+1), membre droit en derniere
	std::vector<size_t> basis;           // variable de base par ligne
	double& At(size_t i, size_t j) { return a[i * (n + 1) + j]; }
	double At(size_t i, size_t j) const { return a[i * (n + 1) + j]; }
};

void Pivot(Tableau& t, size_t row, size_t col) {
	const double p = t.At(row, col);
	for(size_t j = 0; j <= t.n; ++j)
		t.At(row, j) /= p;
	for(size_t i = 0; i < t.m; ++i) {
		if(i == row)
			continue;
		const double f = t.At(i, col);
		if(std::fabs(f) < kEps)
			continue;
		for(size_t j = 0; j <= t.n; ++j)
			t.At(i, j) -= f * t.At(row, j);
	}
	t.basis[row] = col;
}

// Rend faux si le programme est non borne (impossible ici : x est borne ou le
// cout est positif, mais on ne le SUPPOSE pas).
//
// `enterable` (s21) : seules les colonnes j < enterable peuvent ENTRER en base.
// Les artificielles en sont exclues dans les DEUX phases — c'est le resultat
// standard (Chvatal) qui remplace l'ancien cout 1e12 de la phase 2 : le point
// faisable (x*, art = 0) appartient au polyedre restreint, donc l'optimum
// restreint egale l'optimum plein, et une artificielle ne peut jamais rentrer.
// Le cout 1e12 laissait une reentree possible quand une base degeneree portait
// un coefficient > 1 sur la colonne artificielle.
bool Optimize(Tableau& t, std::vector<double>& cost, std::vector<double>& red,
			  size_t enterable) {
	for(;;) {
		// Couts reduits : c_j - c_B B^-1 A_j, calcules en soustrayant les lignes
		// de base (le tableau est deja sous forme canonique).
		red.assign(t.n, 0.0);
		for(size_t j = 0; j < t.n; ++j)
			red[j] = cost[j];
		for(size_t i = 0; i < t.m; ++i) {
			const double cb = cost[t.basis[i]];
			if(std::fabs(cb) < kEps)
				continue;
			for(size_t j = 0; j < t.n; ++j)
				red[j] -= cb * t.At(i, j);
		}
		// BLAND : premiere colonne (plus petit indice) a cout reduit negatif.
		size_t enter = t.n;
		for(size_t j = 0; j < enterable; ++j)
			if(red[j] < -kEps) { enter = j; break; }
		if(enter == t.n)
			return true;   // optimal
		size_t leave = t.m;
		double best = 0.0;
		for(size_t i = 0; i < t.m; ++i) {
			const double aij = t.At(i, enter);
			if(aij <= kEps)
				continue;
			const double ratio = t.At(i, t.n) / aij;
			// BLAND aussi sur la sortie : a ratio egal, plus petit indice de
			// base. C'est ce couple qui interdit le cyclage.
			if(leave == t.m || ratio < best - kEps ||
			   (ratio < best + kEps && t.basis[i] < t.basis[leave])) {
				leave = i;
				best = ratio;
			}
		}
		if(leave == t.m)
			return false;   // non borne
		Pivot(t, leave, enter);
	}
}

}   // namespace

LPResult SolveOperatorLP(const OperatorLP& lp) {
	LPResult res;
	const size_t N = lp.n_ops;
	// Comptage des colonnes : x, puis un surplus/ecart par contrainte, puis un
	// ecart par borne finie, puis les artificielles.
	std::vector<size_t> bound_of;   // indices d'operateurs a borne finie
	for(size_t j = 0; j < N; ++j)
		if(lp.upper[j] < OperatorLP::kNoBound * 0.5)
			bound_of.push_back(j);
	const size_t R = lp.rows.size(), B = bound_of.size();
	// Un membre droit dans (0, kEps] serait traite comme <= 0 et produirait une
	// base de depart LEGEREMENT infaisable (ecart basique negatif). Les membres
	// droits du modele sont entiers ; on ecrase le bruit avant de trier (s21).
	std::vector<double> rhs(R);
	for(size_t i = 0; i < R; ++i)
		rhs[i] = std::fabs(lp.rows[i].rhs) <= kEps ? 0.0 : lp.rows[i].rhs;
	std::vector<char> needs_art(R, 0);
	for(size_t i = 0; i < R; ++i)
		needs_art[i] = rhs[i] > kEps ? 1 : 0;
	size_t n_art = 0;
	for(char c : needs_art)
		n_art += c;

	Tableau t;
	t.m = R + B;
	t.n = N + R + B + n_art;
	t.a.assign(t.m * (t.n + 1), 0.0);
	t.basis.assign(t.m, 0);

	size_t art = N + R + B;
	for(size_t i = 0; i < R; ++i) {
		const double sgn = needs_art[i] ? 1.0 : -1.0;
		for(const auto& [j, v] : lp.rows[i].coef)
			t.At(i, j) += sgn * v;
		t.At(i, N + i) = needs_art[i] ? -1.0 : 1.0;   // surplus ou ecart
		t.At(i, t.n) = sgn * rhs[i];
		if(needs_art[i]) {
			t.At(i, art) = 1.0;
			t.basis[i] = art++;
		} else {
			t.basis[i] = N + i;
		}
	}
	for(size_t k = 0; k < B; ++k) {
		const size_t i = R + k;
		t.At(i, bound_of[k]) = 1.0;
		t.At(i, N + R + k) = 1.0;
		t.At(i, t.n) = lp.upper[bound_of[k]];
		t.basis[i] = N + R + k;
	}

	std::vector<double> cost(t.n, 0.0), red;
	// PHASE 1 : minimiser la somme des artificielles.
	if(n_art) {
		for(size_t j = N + R + B; j < t.n; ++j)
			cost[j] = 1.0;
		if(!Optimize(t, cost, red, N + R + B))
			return res;
		double inf = 0.0;
		for(size_t i = 0; i < t.m; ++i)
			if(t.basis[i] >= N + R + B)
				inf += t.At(i, t.n);
		if(inf > 1e-6)
			return res;   // INFAISABLE : theoreme 3, impasse PROUVEE
		// Chasser les artificielles residuelles de la base (pivot sur toute
		// colonne non artificielle non nulle) ; sinon la phase 2 pourrait les
		// reintroduire.
		for(size_t i = 0; i < t.m; ++i) {
			if(t.basis[i] < N + R + B)
				continue;
			for(size_t j = 0; j < N + R + B; ++j)
				if(std::fabs(t.At(i, j)) > kEps) { Pivot(t, i, j); break; }
		}
	}
	// PHASE 2 : le vrai cout. Les artificielles sont interdites de retour par
	// `enterable` — et celles restees en base apres la chasse ont une ligne
	// nulle sur toute colonne non artificielle (sinon la chasse aurait pivote),
	// donc leur valeur reste a zero quoi qu'il arrive : aucun cout punitif
	// n'est necessaire (s21, derivation en B10).
	std::fill(cost.begin(), cost.end(), 0.0);
	for(size_t j = 0; j < N; ++j)
		cost[j] = lp.cost[j];
	if(!Optimize(t, cost, red, N + R + B))
		return res;

	res.feasible = true;
	res.x.assign(N, 0.0);
	for(size_t i = 0; i < t.m; ++i)
		if(t.basis[i] < N)
			res.x[t.basis[i]] = t.At(i, t.n);
	res.value = 0.0;
	for(size_t j = 0; j < N; ++j)
		res.value += lp.cost[j] * res.x[j];
	// L'ARRONDI SUPERIEUR EST DANS L'ENONCE, PAS UN EMBELLISSEMENT.
	// `h = ceil(c'x*)` (Bonet, def. et th. 2) : les couts sont entiers, donc le
	// cout de tout plan est entier et minore par `c'x*` ; l'arrondi superieur
	// reste donc <= h*, et resserre gratuitement. Le solveur rend des `x`
	// FRACTIONNAIRES (la relaxation continue de l'entier) — mesure sur l'etalon
	// A : `x1.5 renommer`. Sans l'arrondi, `h` annoncerait une valeur qu'aucun
	// plan ne peut realiser.
	// `+ 0.0` : ceil(0 - 1e-9) rend -0.0, qui s'imprime « -0 » ; l'addition le
	// normalise en +0.0 (IEEE, arrondi au plus proche).
	res.value = std::ceil(res.value - 1e-9) + 0.0;

	// --- LES GARDES, ET ELLES SONT LA RAISON D'ETRE DE CE BLOC ---------------
	// Theoremes 1 a 4 ne valent que si CE solveur ne ment pas. On verifie donc
	// la solution rendue contre l'enonce, a chaque appel : faisabilite primale
	// (A'x >= b, 0 <= x <= u) et optimalite (plus aucun cout reduit negatif).
	res.primal_ok = true;
	res.worst_violation = 0.0;
	for(size_t j = 0; j < N; ++j) {
		// La violation est l'EXCES au-dela de la borne, pas la valeur absolue de
		// x : |x| melait « x = 3 pour u = 2 » (violation 1) et « x = 3 sans
		// borne atteinte » (violation 0) dans le meme nombre (s21).
		const double v = (std::max)(-res.x[j], res.x[j] - lp.upper[j]);
		if(v > 1e-6) {
			res.primal_ok = false;
			res.worst_violation = (std::max)(res.worst_violation, v);
		}
	}
	for(const auto& row : lp.rows) {
		double lhs = 0.0;
		for(const auto& [j, v] : row.coef)
			lhs += v * res.x[j];
		if(lhs < row.rhs - 1e-6) {
			res.primal_ok = false;
			res.worst_violation = (std::max)(res.worst_violation,
											 row.rhs - lhs);
		}
	}
	res.optimal_ok = true;
	for(size_t j = 0; j < t.n; ++j)
		if(j < N + R + B && red[j] < -1e-6)
			res.optimal_ok = false;
	return res;
}

namespace {
enum BZone { kRes = 0, kAva = 1, kFld = 2 };
const char* ZoneName(int z) {
	return z == kRes ? "RESERVE" : z == kAva ? "DISPO" : "TERRAIN";
}
}   // namespace

std::vector<uint64_t> BalanceModel::SetcodesOf(uint32_t code) const {
	auto it = sc_cache.find(code);
	if(it != sc_cache.end())
		return it->second;
	std::vector<uint64_t> out;
	if(const CardRow* r = db->Find(code))
		for(uint16_t sc : r->setcodes)
			if(sc)
				out.push_back(sc & 0x0fffu);   // archetype, sous-type ignore
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	sc_cache.emplace(code, out);
	return out;
}

size_t BalanceModel::PlaceId(int kind, uint64_t key, int zone) const {
	const uint64_t k = (uint64_t(kind) << 62) | (key << 2) | uint64_t(zone);
	auto it = pid.find(k);
	return it == pid.end() ? size_t(-1) : it->second;
}

std::string BalanceModel::Produces(size_t t) const {
	if(t >= col.size())
		return std::string();
	for(const auto& kv : col[t])
		if(kv.second > 0)
			return pname[kv.first];
	return std::string();
}

// L'ALIAS, QUE `Canonical` NE REDUIT PAS TOUJOURS. `card::get_code()` du core
// resout `alias` (card.cpp:236) : 90590304 (Bagooska couche) et 90590303 sont la
// MEME carte, et le board le montre. Comparer les codes bruts fabrique deux
// places pour un seul objet — donc un but sans producteur.
static uint32_t AliasOf(const CardDB& db, uint32_t c) {
	const CardRow* r = db.Find(c);
	if(r && r->alias)
		return r->alias;
	return db.Canonical(c);
}

bool BalanceModel::Build(const OperatorTable& tbl, const CardDB& cdb,
						 const ConstantTable& kt,
						 const std::vector<uint32_t>& deck,
						 const std::vector<std::pair<uint32_t, uint32_t>>& goal) {
	db = &cdb;
	if(goal.empty())
		return false;
	auto place = [&](int kind, uint64_t key, int z) -> size_t {
		const uint64_t k = (uint64_t(kind) << 62) | (key << 2) | uint64_t(z);
		auto it = pid.find(k);
		if(it != pid.end())
			return it->second;
		const size_t id = pname.size();
		pid.emplace(k, id);
		char b[96];
		if(kind == 0)
			std::snprintf(b, sizeof b, "%s @%s",
						  cdb.Name(static_cast<uint32_t>(key)).c_str(),
						  ZoneName(z));
		else
			std::snprintf(b, sizeof b, "arch 0x%llx @%s",
						  (unsigned long long)key, ZoneName(z));
		pname.push_back(b);
		need.push_back(0.0);
		return id;
	};
	std::unordered_map<uint32_t, uint32_t> copies;
	for(uint32_t c : deck)
		++copies[AliasOf(cdb, c)];
	// Les places existent des la CONSTRUCTION : le marquage change d'un etat a
	// l'autre, la structure non. C'est ce qui permet de resoudre le meme modele
	// le long d'une ligne sans le rebatir a chaque decision.
	// PLACE GENERIQUE « un monstre disponible » (kind 2, cle 0).
	//
	// `Xyz.AddProcedure(c, nil, 4, 2)` = « 2 monstres de Niveau 4 » : une
	// exigence CARDINALE qui ne nomme ni carte ni archetype, donc invisible aux
	// deux premiers grains. Sans elle, Bagooska n'a AUCUN producteur et le
	// programme entier devient infaisable — mesure faite, et c'est ce que la
	// garde « place SANS PRODUCTEUR » a nomme.
	//
	// Le niveau lui-meme n'est pas porte par `DeclaredRecipe` (seul le COMPTE
	// l'est) : on exige donc « N monstres », pas « N monstres de niveau 4 ».
	// C'est plus FAIBLE que la verite, donc `h` est plus petit, donc
	// l'admissibilite tient. Exiger le niveau sans le lire l'aurait cassee.
	place(2, 0, kRes);
	place(2, 0, kAva);
	for(const auto& kv : copies) {
		place(0, kv.first, kRes);
		place(0, kv.first, kAva);
		place(0, kv.first, kFld);
		for(uint64_t sc : SetcodesOf(kv.first)) {
			place(1, sc, kRes);
			place(1, sc, kAva);
		}
	}
	auto add_tr = [&](const char* what, double cap) {
		lp.cost.push_back(1.0);
		lp.upper.push_back(cap);
		tname.push_back(what);
		return lp.n_ops++;
	};
	auto eff = [&](size_t t, size_t p, double v) {
		if(col.size() <= t)
			col.resize(t + 1);
		col[t][p] += v;
	};
	// (1) LES EFFETS DECLARES, ET RIEN D'AUTRE.
	//
	// LA CORRECTION QUI A COUTE UN ALLER-RETOUR (seance s20). La premiere
	// version portait une transition INVENTEE, `mobiliser` : « toute carte du
	// deck est a une action de la zone disponible ». Elle achetait
	// l'admissibilite par l'optimisme, et le banc a mesure ce qu'elle coutait —
	// 5 % de descentes contre 15 % pour la nouveaute, c'est-a-dire un gradient
	// PIRE que celui qu'elle devait remplacer. La reponse n'est pas de la
	// borner par des regles (« une Invocation Normale par tour ») : ces regles
	// ne portent pas sur elle, et « une carte ne sort du deck que par un effet
	// qui la nomme » est FAUX — une recherche par niveau ne nomme rien, donc la
	// contrainte interdirait de vrais plans et casserait l'admissibilite.
	//
	// SEULS LES EFFETS REELS DES CARTES PRIMENT. Chaque `Duel.SetOperationInfo`
	// declare une paire (categorie, zone source) : c'est une transition, avec sa
	// zone d'origine, sa destination lue dans la categorie, et la capacite de
	// l'effet qui la porte. La place vient du filtre (`IsSetCard`/`IsCode`, via
	// `fn_refs`), sinon des `listed_series`/`listed_names` de la carte, sinon de
	// l'archetype de la carte elle-meme. Aucune de ces sources n'est une regle
	// que nous ajoutons : toutes sont declarees dans le script.
	//
	// CE QUE CELA CHANGE POUR LE THEOREME 1. `h` devient admissible RELATIVEMENT
	// AU MODELE DECLARE : si un effet echappait a l'extraction, un plan reel
	// pourrait violer une ligne. Ce n'est pas une hypothese en l'air — c'est
	// exactement ce que le harnais MESURE (44 activations, 0 non appariee sur
	// `liger.yrpX`). La garantie est donc conditionnee a un nombre qu'on relit
	// a chaque run, et non a une croyance.
	uint64_t c_tohand = 0, c_spsummon = 0, c_tograve = 0, c_todeck = 0,
			 c_search = 0, c_remove = 0;
	kt.Lookup("CATEGORY_TOHAND", c_tohand);
	kt.Lookup("CATEGORY_SPECIAL_SUMMON", c_spsummon);
	kt.Lookup("CATEGORY_TOGRAVE", c_tograve);
	kt.Lookup("CATEGORY_TODECK", c_todeck);
	kt.Lookup("CATEGORY_SEARCH", c_search);
	kt.Lookup("CATEGORY_REMOVE", c_remove);
	for(const auto& kv : tbl.All()) {
		const uint32_t host = kv.first;
		const CardOperators& co = kv.second;
		for(const DeclaredProduct& pr : co.products) {
			// Destination, lue dans la CATEGORIE.
			int dst = -1;
			bool also_field = false;
			if(c_spsummon && (pr.category & c_spsummon)) {
				dst = kAva; also_field = true;
			} else if((c_tohand && (pr.category & c_tohand)) ||
					  (c_search && (pr.category & c_search)) ||
					  (c_tograve && (pr.category & c_tograve)) ||
					  (c_remove && (pr.category & c_remove))) {
				dst = kAva;
			} else if(c_todeck && (pr.category & c_todeck)) {
				dst = kRes;
			}
			if(dst < 0)
				continue;   // categorie qui ne DEPLACE rien : pas une transition
			// Source, lue dans la ZONE declaree. `0` = non declaree : on prend
			// la RESERVE, la lecture la plus utile — donc la plus optimiste,
			// donc celle qui ne peut pas rendre `h` trop grand.
			const int src = (pr.location == 0 || (pr.location & 0x41u))
								? kRes
								: kAva;
			if(src == dst && !also_field)
				continue;   // ne bouge rien dans nos trois zones
			// La PLACE : le filtre d'abord, la carte ensuite. Jamais une
			// supposition.
			std::vector<uint64_t> arch;
			std::vector<uint32_t> named;
			std::vector<std::string> scope{ pr.in_function };
			auto rit = co.fn_refs.find(pr.in_function);
			if(rit != co.fn_refs.end())
				for(const std::string& r : rit->second)
					scope.push_back(r);
			for(const std::string& f : scope) {
				auto sit = co.fn_setcodes.find(f);
				if(sit != co.fn_setcodes.end())
					for(uint64_t v : sit->second)
						arch.push_back(v & 0x0fffull);
				auto cit2 = co.fn_codes.find(f);
				if(cit2 != co.fn_codes.end())
					for(uint32_t v : cit2->second)
						named.push_back(cdb.Canonical(v));
			}
			if(arch.empty() && named.empty()) {
				for(uint64_t v : co.listed_series)
					arch.push_back(v & 0x0fffull);
				for(uint32_t v : co.listed_names)
					named.push_back(cdb.Canonical(v));
			}
			if(arch.empty() && named.empty())
				for(uint64_t v : SetcodesOf(host))
					arch.push_back(v);
			if(arch.empty() && named.empty())
				continue;
			// Capacite : celle de l'effet qui porte cette fonction, multipliee
			// par les copies quand elle est « par COPIE ».
			double cap = OperatorLP::kNoBound;
			for(const std::vector<DeclaredEffect>* set :
				{ &co.operators, &co.grants }) {
				for(const DeclaredEffect& e : *set) {
					const bool mine =
						e.fn_cost == pr.in_function ||
						e.fn_condition == pr.in_function ||
						e.fn_target == pr.in_function ||
						e.fn_operation == pr.in_function ||
						("s." + pr.in_function) == e.fn_target ||
						("s." + pr.in_function) == e.fn_operation;
					if(mine && e.has_count_limit) {
						const double n = copies.count(host)
											 ? double(copies.at(host))
											 : 1.0;
						cap = e.count_by_name ? double(e.count_limit)
											  : double(e.count_limit) * n;
					}
				}
			}
			const size_t t = add_tr("effet", cap);
			for(uint32_t nc : named) {
				eff(t, place(0, nc, src), -1.0);
				eff(t, place(0, nc, dst), +1.0);
				if(also_field)
					eff(t, place(0, nc, kFld), +1.0);
			}
			if(named.empty())
				for(uint64_t a : arch) {
					eff(t, place(1, a, src), -1.0);
					eff(t, place(1, a, dst), +1.0);
				}
		}
	}
	// (2) INVOQUER : la recette DECLAREE, avec sa MULTIPLICITE.
	for(const auto& kv : tbl.All()) {
		const uint32_t c = kv.first;
		const CardOperators& co = kv.second;
		if(co.recipes.empty())
			continue;
		const DeclaredRecipe& r = co.recipes.front();
		// `unresolved_counts` COMPTE : « 2 monstres de Niveau 4 » est une
		// recette complete. L'omettre du garde privait Bagooska — et tout Xyz —
		// de producteur, donc rendait le programme entier infaisable.
		if(r.named.empty() && r.setcode.empty() && r.unresolved_counts.empty())
			continue;
		// LE PRODUIT SE CANONISE, comme le but. `--target 90590304` nomme la
		// forme couchee de Bagooska ; la table indexe `90590303`. Sans cette
		// reduction les deux places sont distinctes, la recette ne produit rien
		// pour le but, et le programme entier devient infaisable — mesure faite.
		const uint32_t cc = AliasOf(cdb, c);
		const size_t t = add_tr("invoquer", OperatorLP::kNoBound);
		eff(t, place(0, cc, kRes), -1.0);
		eff(t, place(0, cc, kFld), +1.0);
		eff(t, place(0, cc, kAva), +1.0);
		for(const auto& m : r.named)
			eff(t, place(0, cdb.Canonical(m.first), kAva),
				-static_cast<double>(m.second));
		for(const auto& sc : r.setcode)
			eff(t, place(1, sc.first & 0x0fffull, kAva),
				-static_cast<double>(sc.second));
		for(uint32_t k : r.unresolved_counts)
			eff(t, place(2, 0, kAva), -static_cast<double>(k));
	}
	// (3) RENOMMER : la famille parametree d'EFFECT_ADD_CODE. Le code accorde
	// est `e:GetLabel()`, pose au COUT depuis la carte envoyee au cimetiere —
	// d'ou +2 en DISPO : le porteur renomme compte comme X pour une Fusion, ET
	// la carte envoyee y est rendue materiau par EFFECT_EXTRA_FUSION_MATERIAL.
	uint64_t add_code = 0;
	kt.Lookup("EFFECT_ADD_CODE", add_code);
	for(const auto& kv : tbl.All()) {
		const uint32_t host = kv.first;
		bool grants = false;
		for(const DeclaredEffect& g : kv.second.grants)
			grants = grants || (g.code_is_effect && add_code &&
								g.code_value == add_code);
		if(!grants)
			continue;
		const double cap = copies.count(host)
							   ? static_cast<double>(copies.at(host))
							   : 0.0;
		if(cap <= 0.0)
			continue;
		const std::vector<uint64_t> hsc = SetcodesOf(host);
		for(const auto& cx : copies) {
			const uint32_t x = cx.first;
			if(x == host)
				continue;
			bool share = false;
			for(uint64_t sc : SetcodesOf(x))
				share = share ||
						std::find(hsc.begin(), hsc.end(), sc) != hsc.end();
			if(!share)
				continue;
			const size_t t = add_tr("renommer", cap);
			eff(t, place(0, x, kRes), -1.0);
			eff(t, place(0, x, kAva), +2.0);
			++n_rename;
		}
	}
	{
		const size_t gen = place(2, 0, kAva);
		for(size_t t = 0; t < col.size(); ++t) {
			double pos = 0.0;
			for(const auto& kv : col[t])
				if(kv.first != gen && kv.second > 0)
					pos = (std::max)(pos, kv.second);
			if(pos > 0)
				col[t][gen] += pos;
		}
	}
	for(const auto& g : goal) {
		need[place(0, AliasOf(cdb, g.first), kFld)] += g.second;
		goal_codes.push_back(AliasOf(cdb, g.first));
	}
	col.resize(lp.n_ops);
	return true;
}

double BalanceModel::Solve(const std::vector<uint32_t>& res,
						   const std::vector<uint32_t>& ava,
						   const std::vector<uint32_t>& fld,
						   LPResult* out) const {
	std::vector<double> mark(pname.size(), 0.0);
	auto put = [&](const std::vector<uint32_t>& codes, int z) {
		for(uint32_t raw : codes) {
			const uint32_t c = db->Canonical(raw);
			size_t p = PlaceId(0, c, z);
			if(p != static_cast<size_t>(-1))
				mark[p] += 1.0;
			if(z == kFld)
				continue;   // les archetypes ne sont suivis qu'en RES et AVA
			if(const CardRow* row = db->Find(c))
				if(row->type & 0x1u) {   // TYPE_MONSTER
					p = PlaceId(2, 0, z);
					if(p != static_cast<size_t>(-1))
						mark[p] += 1.0;
				}
			for(uint64_t sc : SetcodesOf(c)) {
				p = PlaceId(1, sc, z);
				if(p != static_cast<size_t>(-1))
					mark[p] += 1.0;
			}
		}
	};
	put(res, kRes);
	put(ava, kAva);
	put(fld, kFld);

	OperatorLP inst;
	inst.n_ops = lp.n_ops;
	inst.cost = lp.cost;
	inst.upper = lp.upper;
	for(size_t p = 0; p < pname.size(); ++p) {
		OperatorLP::Row row;
		for(size_t t = 0; t < col.size(); ++t) {
			auto it = col[t].find(p);
			if(it != col[t].end() && std::fabs(it->second) > 1e-12)
				row.coef.emplace_back(t, it->second);
		}
		const double b = need[p] - mark[p];
		if(row.coef.empty() && b <= 0.0)
			continue;
		row.rhs = b;
		row.label = pname[p];
		inst.rows.push_back(std::move(row));
	}
	// DEUX INFAISABILITES QU'IL NE FAUT PAS CONFONDRE, et les confondre rendrait
	// le theoreme 3 inutilisable.
	//
	//   « ce but est PROUVE hors d'atteinte »  (copies, capacites : un fait)
	//   « je ne sais pas fabriquer cette place » (aucune transition ne la
	//                                             produit : une LACUNE du modele)
	//
	// La seconde se detecte avant de resoudre, et coute une boucle : une ligne a
	// membre droit positif dont AUCUN coefficient n'est positif n'a pas de
	// producteur. L'annoncer comme une preuve serait un mensonge ; l'annoncer
	// comme un trou d'extraction en fait une liste de travail.
	for(const OperatorLP::Row& row : inst.rows) {
		if(row.rhs <= 1e-9)
			continue;
		bool producible = false;
		for(const auto& kv : row.coef)
			producible = producible || kv.second > 0;
		if(!producible)
			std::printf("!! place SANS PRODUCTEUR : %s (exige %.0f) — lacune "
						"d'extraction, PAS une preuve d'impossibilite\n",
						row.label.c_str(), row.rhs);
	}
	LPResult r = SolveOperatorLP(inst);
	if(out)
		*out = r;
	if(!r.feasible || !r.primal_ok || !r.optimal_ok)
		return -1.0;
	return r.value;
}

std::vector<BalanceModel::Need> BalanceModel::NeedsFrom(
	const LPResult& r) const {
	std::vector<Need> out;
	if(!r.feasible)
		return out;
	// Agrege par place PRODUITE : combien de jetons chaque transition qui tire
	// doit y deposer. L'arrondi est SUPERIEUR — une demi-invocation n'existe
	// pas, et sous-compter un sous-but le rendrait franchissable a moitie.
	std::unordered_map<size_t, double> want;
	for(size_t t = 0; t < r.x.size() && t < col.size(); ++t) {
		if(r.x[t] <= 1e-6)
			continue;
		for(const auto& kv : col[t])
			if(kv.second > 0)
				want[kv.first] += kv.second * r.x[t];
	}
	for(const auto& kv : pid) {
		auto it = want.find(kv.second);
		if(it == want.end() || it->second < 0.5)
			continue;
		Need n;
		const uint64_t key = kv.first;
		const int kind = static_cast<int>(key >> 62);
		n.zone = static_cast<uint8_t>(key & 3u);
		const uint64_t k = (key >> 2) & ((1ull << 60) - 1);
		if(kind == 0)
			n.code = static_cast<uint32_t>(k);
		else
			n.arch = k;
		n.count = static_cast<uint32_t>(std::ceil(it->second - 1e-9));
		out.push_back(n);
	}
	return out;
}

std::vector<BalanceModel::Need> BalanceModel::ConsumedFrom(
	const LPResult& r) const {
	std::vector<Need> out;
	if(!r.feasible)
		return out;
	// Consommation totale par place : Sigma_t x_t * (-coef negatif).
	std::unordered_map<size_t, double> eaten;
	for(size_t t = 0; t < r.x.size() && t < col.size(); ++t) {
		if(r.x[t] <= 1e-6)
			continue;
		for(const auto& kv : col[t])
			if(kv.second < 0)
				eaten[kv.first] += -kv.second * r.x[t];
	}
	// Agregee par IDENTITE (kind, cle), zones confondues : un corps consomme
	// depuis la RESERVE (renommage : deck -> cimetiere) et un corps consomme
	// depuis DISPO (materiau de fusion) atterrissent au meme endroit. La place
	// generique (kind 2) est ecartee : « un monstre quelconque au cimetiere »
	// ne se compte pas sans double emploi avec les identites nommees.
	std::unordered_map<uint64_t, double> merged;
	for(const auto& kv : pid) {
		auto it = eaten.find(kv.second);
		if(it == eaten.end())
			continue;
		if(static_cast<int>(kv.first >> 62) == 2)
			continue;
		merged[kv.first & ~3ull] += it->second;
	}
	for(const auto& kv : merged) {
		if(kv.second < 0.5)
			continue;
		Need n;
		const int kind = static_cast<int>(kv.first >> 62);
		const uint64_t k = (kv.first >> 2) & ((1ull << 60) - 1);
		// JAMAIS un barreau de consommation sur une carte DU BUT (s21).
		// x* peut consommer un Liger (descost) — mais recompenser « Liger au
		// cimetiere » quand le but est « 3 Liger » et que le deck n'en a que 3
		// fabrique des cellules en progres APPARENT qui ont detruit le but.
		// C'est mesure : la ligne reelle ne sert jamais ce barreau, les
		// tirages du run nu le servaient massivement, et le tournoi preferait
		// ces cellules empoisonnees.
		if(kind == 0 &&
		   std::find(goal_codes.begin(), goal_codes.end(),
					 static_cast<uint32_t>(k)) != goal_codes.end())
			continue;
		if(kind == 0)
			n.code = static_cast<uint32_t>(k);
		else
			n.arch = k;
		n.zone = 3;   // CIMETIERE
		// Plafond a 15 : l'empaquetage de SerialProgress porte 4 bits par
		// exigence, et un barreau au-dela du plafond n'existerait pas.
		n.count = (std::min)(
			static_cast<uint32_t>(std::ceil(kv.second - 1e-9)), 15u);
		out.push_back(n);
		// LES BARREAUX @BANNIE ONT ETE RETIRES (s21, observation operateur +
		// deux mesures). (1) Au banc, ils n'apportaient AUCUN barreau dans les
		// deserts : les bannissements de fusion tombent en rafale A
		// l'invocation, la meme reponse que les barreaux Liger@DISPO/TERRAIN
		// deja servis. (2) Au run, ils etaient un CANAL DE POISON massif : la
		// negation de Silver Hound (QUICK depuis le cimetiere) bannit en COUT
		// Silver + une Fusion Lunalight du cimetiere — le Leo-materiau. Leo
		// atteignait la zone bannie dans 25 884 tirages pour 260 invocations
		// de Liger (98,9 % de suicides recompenses par l'echelle). Le CHOIX de
		// Silver reste jouable (regle 2) ; seule la recompense est retiree —
		// meme geste que Liger@CIMETIERE.
	}
	return out;
}

double BuildAndSolveBalance(
	const OperatorTable& tbl, const CardDB& db, const ConstantTable& kt,
	const std::vector<uint32_t>& deck,
	const std::vector<std::pair<uint32_t, uint32_t>>& goal) {
	std::printf("\n=== LE BILAN MATIERE : h(depart) ===\n");
	BalanceModel m;
	if(!m.Build(tbl, db, kt, deck, goal)) {
		std::printf("  (aucun but donne)\n");
		return -1.0;
	}
	std::printf("  places %zu, transitions %zu (dont %zu renommages)\n",
				m.Places(), m.Transitions(), m.Renames());
	LPResult r;
	// Etat de depart : tout le deck est en RESERVE. C'est l'etat AVANT la
	// pioche, celui qui repond a « ce deck peut-il, en principe ? ».
	const double h = m.Solve(deck, std::vector<uint32_t>(),
							 std::vector<uint32_t>(), &r);
	if(!r.feasible) {
		std::printf("  h = INFINI  —  IMPASSE PROUVEE (theoreme 3) : aucun "
					"plan n'atteint ce but depuis ce deck.\n");
		return -1.0;
	}
	std::printf("  h(depart) = %.0f   [gardes : primal %s, optimal %s]\n",
				r.value, r.primal_ok ? "OK" : "VIOLE",
				r.optimal_ok ? "OK" : "VIOLE");
	if(h < 0) {
		std::printf("!! LE SOLVEUR S'EST CONTREDIT — valeur a jeter "
					"(garde 9.30)\n");
		return -1.0;
	}
	std::printf("  --- le vecteur de tirs x (non nuls) ---\n");
	std::vector<std::pair<double, size_t>> nz;
	for(size_t t = 0; t < m.Transitions(); ++t)
		if(r.x[t] > 1e-6)
			nz.emplace_back(r.x[t], t);
	std::sort(nz.rbegin(), nz.rend());
	for(size_t i = 0; i < nz.size() && i < 20; ++i)
		std::printf("   x%-5.1f %-11s %s\n", nz[i].first,
					m.TrNames()[nz[i].second].c_str(),
					m.Produces(nz[i].second).c_str());
	return h;
}

size_t SelfTestOperatorLP(size_t* total) {
	// CINQ INSTANCES A SOLUTION CONNUE A LA MAIN. Elles couvrent exactement les
	// quatre theoremes : multiplicite (le but a 3 exemplaires), consommation
	// (une transition qui detruit), capacite (theoreme 4) et infaisabilite
	// (theoreme 3). Un solveur qui les passe toutes n'est pas prouve juste ;
	// un solveur qui en rate une est prouve faux, et c'est ce qu'on veut.
	struct Case { OperatorLP lp; bool feas; double val; };
	std::vector<Case> cs;
	auto mk = [](size_t n) {
		OperatorLP lp;
		lp.n_ops = n;
		lp.cost.assign(n, 1.0);
		lp.upper.assign(n, OperatorLP::kNoBound);
		return lp;
	};
	{   // 1. un seul operateur produit le but : x >= 3  ->  h = 3
		Case c; c.lp = mk(1);
		c.lp.rows.push_back({ { { 0, 1.0 } }, 3.0, "but" });
		c.feas = true; c.val = 3.0; cs.push_back(c);
	}
	{   // 2. consommation : o1 produit p et consomme q, o2 produit q.
		//    but p >= 2  =>  x1 = 2, et q descend a -2 donc x2 >= 2  =>  h = 4.
		Case c; c.lp = mk(2);
		c.lp.rows.push_back({ { { 0, 1.0 } }, 2.0, "p" });
		c.lp.rows.push_back({ { { 0, -1.0 }, { 1, 1.0 } }, 0.0, "q" });
		c.feas = true; c.val = 4.0; cs.push_back(c);
	}
	{   // 3. capacite (th. 4) : deux voies, la moins chere plafonnee a 1.
		Case c; c.lp = mk(2);
		c.lp.cost = { 1.0, 5.0 };
		c.lp.upper = { 1.0, OperatorLP::kNoBound };
		c.lp.rows.push_back({ { { 0, 1.0 }, { 1, 1.0 } }, 3.0, "but" });
		c.feas = true; c.val = 1.0 + 2.0 * 5.0; cs.push_back(c);
	}
	{   // 4. infaisable (th. 3) : le but exige 3, la seule voie est plafonnee a 2.
		Case c; c.lp = mk(1);
		c.lp.upper = { 2.0 };
		c.lp.rows.push_back({ { { 0, 1.0 } }, 3.0, "but" });
		c.feas = false; c.val = 0.0; cs.push_back(c);
	}
	{   // 5. contrainte deja satisfaite (membre droit negatif) : n'impose rien.
		Case c; c.lp = mk(1);
		c.lp.rows.push_back({ { { 0, 1.0 } }, 1.0, "but" });
		c.lp.rows.push_back({ { { 0, -1.0 } }, -4.0, "reserve" });
		c.feas = true; c.val = 1.0; cs.push_back(c);
	}
	// LE FUZZ (s21) : 60 instances aleatoires de la meme forme, resolues en
	// rationnels EXACTS par sympy.lpmin (verify_formulas.py, B10) — dont 20
	// infaisables. Cinq cas a la main prouvent la couverture des theoremes ;
	// soixante cas tires au sort prouvent le SOLVEUR, membre droit negatif,
	// bornes actives et degenerescence compris. La valeur attendue est
	// ceil(optimum exact), ce que rend SolveOperatorLP.
	#include "lp_fuzz_cases.inc"
	for(const FuzzCase& f : kFuzzCases) {
		Case c;
		c.lp.n_ops = f.n;
		c.lp.cost.assign(f.cost, f.cost + f.n);
		for(size_t j = 0; j < f.n; ++j)
			c.lp.upper.push_back(f.upper[j] < 0 ? OperatorLP::kNoBound
												: f.upper[j]);
		for(size_t i = 0; i < f.nrows; ++i) {
			OperatorLP::Row row;
			for(size_t j = 0; j < f.n; ++j)
				if(f.coef[i][j] != 0.0)
					row.coef.emplace_back(j, f.coef[i][j]);
			row.rhs = f.rhs[i];
			row.label = "fuzz";
			c.lp.rows.push_back(std::move(row));
		}
		c.feas = f.feas;
		c.val = f.val;
		cs.push_back(std::move(c));
	}
	size_t pass = 0;
	for(size_t ci = 0; ci < cs.size(); ++ci) {
		const Case& c = cs[ci];
		LPResult r = SolveOperatorLP(c.lp);
		bool ok = r.feasible == c.feas;
		if(ok && c.feas)
			ok = std::fabs(r.value - c.val) < 1e-6 && r.primal_ok &&
				 r.optimal_ok;
		if(ok)
			++pass;
		else
			std::printf("!! AUTO-TEST LP : cas #%zu « %s » attendu %s %.2f, rendu "
						"%s %.2f (primal %d, optimal %d)\n", ci,
						c.lp.rows.empty() ? "?" : c.lp.rows[0].label.c_str(),
						c.feas ? "faisable" : "INFAISABLE", c.val,
						r.feasible ? "faisable" : "INFAISABLE", r.value,
						r.primal_ok ? 1 : 0, r.optimal_ok ? 1 : 0);
	}
	if(total)
		*total = cs.size();
	return pass;
}

void OperatorTable::PrintFiringCounts(
	const CardDB& db, const ConstantTable& kt,
	const std::vector<uint32_t>& deck,
	const std::vector<std::pair<uint32_t, uint32_t>>& goal) const {
	std::printf("\n=== MARCHE 1 : LES COMPTES DE TIR (multiplicite x capacite) "
				"===\n");
	if(goal.empty()) {
		std::printf("  (aucun but donne : ajouter --target)\n");
		return;
	}
	// Copies PHYSIQUES par code. C'est ce nombre, et non la presence, qui
	// decide d'une capacite « par COPIE » : trois Kaleido Chick valent trois
	// activations par tour, un seul n'en vaut qu'une.
	std::unordered_map<uint32_t, uint32_t> copies;
	for(uint32_t c : deck)
		++copies[db.Canonical(c)];

	// Ce qu'il faut PRODUIRE, et combien de fois. `fire[code]` est le nombre de
	// tirs de l'operateur d'invocation de `code` : c'est le `x_o` du bilan.
	std::unordered_map<uint32_t, uint32_t> fire, need_named;
	std::map<uint64_t, uint32_t> need_setcode;
	// LE BUT SE CANONISE A L'ENTREE. `--target 90590304` nomme la forme
	// « couchee » de Bagooska ; la table est batie sur les codes du deck, donc
	// sur `90590303`. Sans cette reduction, la carte ne trouve aucune recette et
	// disparait des comptes EN SILENCE — un but sur deux non compte, sans un
	// mot. C'est le meme piege que partout ailleurs dans ce dossier : ce qui ne
	// s'apparie pas doit se VOIR, et ici il suffit de ne pas creer l'ecart.
	std::vector<std::pair<uint32_t, uint32_t>> work;
	for(const auto& [gc, gn] : goal)
		work.emplace_back(db.Canonical(gc), gn);
	std::unordered_map<uint32_t, uint32_t> seen_depth;
	for(size_t guard = 0; !work.empty() && guard < 4096; ++guard) {
		const auto [code, n] = work.back();
		work.pop_back();
		if(!n)
			continue;
		if(++seen_depth[code] > 8)   // graphe cyclique : on borne, on le dit
			continue;
		need_named[code] += n;
		auto it = cards.find(code);
		if(it == cards.end() || it->second.recipes.empty())
			continue;   // carte de base : rien a fabriquer
		// UNE SEULE VOIE DEVELOPPEE, et c'est dit dans l'en-tete : on prend la
		// recette declaree, on ne choisit pas a la place du jeu.
		const DeclaredRecipe& r = it->second.recipes.front();
		if(r.named.empty() && r.setcode.empty() && r.unresolved_counts.empty())
			continue;
		fire[code] += n;
		for(const auto& [m, k] : r.named)
			work.emplace_back(db.Canonical(m), n * k);
		for(const auto& [s, k] : r.setcode)
			need_setcode[s] += n * k;
	}

	std::printf("\n  --- CE QUI DOIT TIRER, ET COMBIEN DE FOIS ---\n");
	std::vector<std::pair<uint32_t, uint32_t>> rows(fire.begin(), fire.end());
	std::sort(rows.begin(), rows.end(),
			  [&](const auto& a, const auto& b) { return a.second > b.second; });
	for(const auto& [code, n] : rows)
		std::printf("  x%-3u  invocation de %-34s (%u copie(s) au deck)\n", n,
					db.Name(code).c_str(), copies[code]);
	for(const auto& [s, n] : need_setcode)
		std::printf("  x%-3u  CORPS d'archetype 0x%llx  (exigence cardinale)\n",
					n, (unsigned long long)s);

	// CE QUI N'ENTRE PAS DANS LES COMPTES DOIT SE VOIR. Un but dont aucune
	// recette n'a ete extraite disparaissait SANS UN MOT du bilan — et un bilan
	// qui compte la moitie d'un but se lit comme un bilan complet. La cause est
	// dite, pas devinee : ou la carte n'a pas de recette declaree, ou son code
	// ne se reduit pas a celui du deck.
	for(const auto& [gc, gn] : goal) {
		const uint32_t c = db.Canonical(gc);
		if(fire.count(c))
			continue;
		auto it = cards.find(c);
		std::printf("  --    %-34s x%u : HORS COMPTES (%s)\n",
					db.Name(c).c_str(), gn,
					it == cards.end()
						? "code absent de la table — alias non reduit"
						: "aucune recette declaree extraite");
	}

	// LA LIGNE DU BILAN, PAR PRODUIT — et c'est ICI que l'objection « le deck
	// RECYCLE » devient un NOMBRE au lieu d'un argument.
	//
	// Un monstre d'extra ne peut etre invoque qu'autant de fois qu'il en existe
	// de copies... PLUS ce que les recycleurs remettent dans la zone. Compter
	// les seules copies serait un raisonnement de STOCK, c'est-a-dire l'erreur
	// exacte que l'equation de bilan existe pour ne pas commettre. On compte
	// donc un FLUX : copies + capacite de recyclage >= tirs exiges.
	uint64_t zextra = 0;
	kt.Lookup("LOCATION_EXTRA", zextra);
	uint32_t recyc = 0;
	bool recyc_unbounded = false;
	for(const auto& [host, co] : cards) {
		for(const auto& [fn, verbs] : co.fn_verbs) {
			auto lit = co.fn_locations.find(fn);
			if(lit == co.fn_locations.end() || !(lit->second & zextra))
				continue;
			bool rec = false;
			for(const std::string& v : verbs)
				rec = rec || ClassifyVerb(v) == VerbKind::kRecycle;
			if(!rec)
				continue;
			const DeclaredEffect* owner = OwnerOf(co, fn);
			if(!owner || !owner->has_count_limit) {
				recyc_unbounded = true;   // direction SURE : jamais d'elagage
				continue;
			}
			const uint32_t nb = copies.count(host) ? copies.at(host) : 0u;
			recyc += owner->count_by_name ? owner->count_limit
										  : owner->count_limit * nb;
		}
	}
	std::printf("\n  --- LA LIGNE DU BILAN, PAR PRODUIT (copies + recyclage "
				">= tirs) ---\n");
	std::printf("  capacite de RECYCLAGE sur l'extra : %u/tour%s\n", recyc,
				recyc_unbounded ? " + au moins un recycleur SANS BORNE declaree"
								  " (garde par une fermeture : on ne conclut"
								  " pas)" : "");
	size_t n_tight = 0, n_dead = 0;
	for(const auto& [code, n] : rows) {
		const uint32_t have = copies.count(code) ? copies.at(code) : 0u;
		if(have >= n)
			continue;
		const uint32_t manque = n - have;
		const bool dead = !recyc_unbounded && recyc < manque;
		if(dead) ++n_dead; else ++n_tight;
		std::printf("  %-34s tirs x%u, copies %u  =>  il MANQUE %u, "
					"a servir par recyclage   %s\n",
					db.Name(code).c_str(), n, have, manque,
					dead ? "<<< LIGNE INFAISABLE" : "TENDU");
	}
	if(!n_tight && !n_dead)
		std::printf("  (aucun produit ne depasse ses copies : la ligne ne "
					"contraint rien ici)\n");

	// LES ACQUISITIONS, ET C'EST LA QUE LA CAPACITE TRANCHE. Un materiau NOMME
	// absent du deck ne peut venir que d'un etat accorde `EFFECT_ADD_CODE` — et
	// l'operateur qui l'accorde a une capacite DECLAREE. Trois exemplaires du
	// but demandent trois acquisitions ; une capacite « par NOM » en autorise
	// UNE, quelles que soient les copies. C'est une ligne du bilan, et elle se
	// tranche sans un seul tirage.
	uint64_t add_code = 0;
	kt.Lookup("EFFECT_ADD_CODE", add_code);
	std::printf("\n  --- LES ACQUISITIONS (materiau NOMME absent du deck) ---\n");
	size_t n_acq = 0, n_bad = 0;
	for(const auto& [code, n] : need_named) {
		// Present au deck (main OU extra) : il existe physiquement, rien a
		// acquerir. S'il est present mais NON INVOCABLE, c'est la recursion qui
		// le dira — en butant sur SON materiau nomme, absent lui.
		if(copies.count(code))
			continue;
		++n_acq;
		bool served = false;
		for(const auto& [host, co] : cards) {
			for(const DeclaredEffect& g : co.grants) {
				if(!g.code_is_effect || g.code_value != add_code || !add_code)
					continue;
				const DeclaredEffect* owner = OwnerOf(co, NormFn(g.in_function));
				const uint32_t nb = copies.count(host) ? copies.at(host) : 0u;
				uint32_t cap = 0;
				const char* how = "SANS BORNE";
				if(owner && owner->has_count_limit) {
					cap = owner->count_by_name ? owner->count_limit
											   : owner->count_limit * nb;
					how = owner->count_by_name ? "par NOM" : "par COPIE";
				}
				const bool ok = !owner || !owner->has_count_limit || cap >= n;
				std::printf("  %-30s <- %-28s  requis x%u,  capacite %u %s"
							" (%u copie(s))   %s\n",
							db.Name(code).c_str(), db.Name(host).c_str(), n,
							cap, how, nb, ok ? "OK" : "<<< INFAISABLE");
				if(!ok)
					++n_bad;
				served = true;
			}
		}
		if(!served)
			std::printf("  %-30s <- AUCUN etat accorde ne le rend  requis x%u"
						"   <<< INFAISABLE (ligne vide)\n",
						db.Name(code).c_str(), n);
	}
	if(!n_acq)
		std::printf("  (aucune : tout materiau nomme est au deck)\n");

	std::printf("\n  VERDICT DE LA LIGNE : %zu acquisition(s) exigee(s), "
				"%zu au-dela de la capacite declaree.\n", n_acq, n_bad);
	std::printf("  Ce verdict est NECESSAIRE, jamais suffisant : une voie "
				"declaree est developpee,\n"
				"  les conditions restent des fermetures, et l'ordre n'y entre "
				"pas. Un `INFAISABLE`\n"
				"  est donc une preuve ; un `OK` n'est qu'une absence de "
				"preuve du contraire.\n");
}

} // namespace solver
