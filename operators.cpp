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

// --- READING TOOLKIT ---------------------------------------------------------

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

// Strips the Lua comments from a line. Block comments `--[[ ]]` are handled by
// the caller (they span lines); here, only the end-of-line `--`. String
// literals are respected: a `--` inside "a--b" is not one.
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

// Text between the opening parenthesis following `pos` and its closing one.
// Returns false when the parenthesis does not close on the line, in which case
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

// Splits an argument list on the TOP-LEVEL commas.
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

// Occurrence of a `name(` call outside any string, starting at `from`.
size_t FindCall(const std::string& s, const std::string& name, size_t from = 0) {
	while(from < s.size()) {
		size_t p = s.find(name, from);
		if(p == std::string::npos)
			return std::string::npos;
		// A call is not the suffix of an identifier: `xSetRange` is not
		// `SetRange`. We require the preceding character not to be an
		// identifier character, except when the name starts with `:` or `.`.
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

// --- CONSTANTS ---------------------------------------------------------------

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
		// Neither `==`, nor `<=`, nor `~=`: this file only contains
		// assignments, but we do not want a silent false positive.
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
// Two things are read there: how far the code is shifted, and how many bits the
// index occupies. Nothing else of the body is interpreted; if the shape
// changes, `string_shift_read` stays false and the caller SAYS so.
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
	// The explicit mask, when written, wins: `id & 0xfffff`.
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
	// ORDER MATTERS: `constant.lua` first, because the other files draw on it (a
	// composite value only evaluates once its terms are already there).
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
	// Deliberately POOR grammar: tokens separated by `+`, `|`, `~` (flag
	// concatenation) and nothing else. An expression outside that grammar is
	// REFUSED, never approximated; an invented value would manufacture an operator
	// that does not exist.
	std::string s = Trim(expr);
	if(s.empty())
		return false;
	// A comma or a call: outside the grammar.
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
		// A single bit: this is a flag, not a composite alias.
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

// --- SYMBOLIC ZONES ----------------------------------------------------------

uint64_t NormalizeRange(uint64_t range) {
	if(!range)
		return 0;
	uint64_t out = range & 0xffu;   // physical zones pass through unchanged
	if(range & 0x100u) out |= 0x8u;   // FZONE  -> SZONE
	if(range & 0x200u) out |= 0x8u;   // PZONE  -> SZONE
	if(range & 0x400u) out |= 0x8u;   // STZONE -> SZONE
	if(range & 0x800u) out |= 0x4u;   // MMZONE -> MZONE
	if(range & 0x1000u) out |= 0x4u;  // EMZONE -> MZONE
	return out;
}

// --- STATIC ANALYSIS OF A SCRIPT ---------------------------------------------

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

	// `local s,id=GetID()`: `id` IS the card's code. We do not assume it, we read
	// it; a shared script (alias, variants) would say it differently.
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

		// Function context. `function s.NAME(` opens it, an `end` in column 0
		// closes it. That is crude and SUFFICIENT: card scripts do not nest
		// named functions at the top level.
		//
		// THE NAME STAYS QUALIFIED outside card scripts. A card script prefixes
		// everything with `s.` (or `cXXXX.`) and we strip that prefix; a
		// `proc_*.lua` file declares `Pendulum.AddProcedure`, and it is that WHOLE
		// name the card calls. Truncating it would conflate the homonymous
		// procedures of two summon families.
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
				// is a procedure declared by ASSIGNMENT. Without this case, the
				// operators of the procedures would be invisible. The search is
				// CASE-INSENSITIVE: the factory is called `FunctionWithNamedArgs`,
				// and a case-sensitive `find("function")` missed EVERY procedure
				// declared that way.
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
			// nothing to extract: `id` is the file's code by construction
			(void)script_id;
		}

		// --- creating an effect ----------------------------------------------
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
		// `local e2=e1:Clone()`: a clone INHERITS everything, the description
		// included. Ignoring it would lose whole operators (Leo has one).
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

		// --- setters on a known effect ---------------------------------------
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

		// --- registration -----------------------------------------------------
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
		// `e:GetHandler():RegisterEffect(e1)` and variants: we catch them by the
		// suffix, otherwise granted states would vanish from the table.
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

		// --- what the operator PRODUCES --------------------------------------
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

		// --- declared strings (the identity of the prompts) -------------------
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
						// The SITE, i.e. what the prompt WILL BE. It is read from the
						// text preceding the call: `SetDescription`,
						// `Duel.SelectYesNo`, `Duel.SelectOption`, a hint...
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

		// --- summon procedures (THE RECIPES, in machine form) ----------------
		//
		// EVERY `<Family>.AddProc*` procedure is recorded, rather than a closed
		// list: that is the route taken both by a Fusion's recipe and by
		// SETTING A PENDULUM SCALE or activating a Polymerization, whose
		// operator does not live in the card's script but in `proc_*.lua`.
		//
		// EVERY `<Family>.<Method>(` CALL INSIDE `initial_effect` IS A
		// PROCEDURE, not only `AddProc*`: Polymerization declares nothing but
		// `Fusion.RegisterSummonEff(c)`, and its activation operator lives two
		// levels further down. The excluded families are those of the API
		// itself (`Effect`, `Duel`, `Card`, `Group`); sticking to a list of
		// allowed METHODS would itself be a catalogue.
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
			// `Fusion.AddProcMixN(c, sub, insub, m1, n1, m2, n2, ...)`: the
			// materials come in PAIRS (filter, count). A numeric literal filter
			// is a card CODE; `IsSetCard(SET_x)` is an archetype; everything
			// else is an unnamed cardinal.
			//
			// The other procedures carry no pairs: we derive NO requirement from
			// them rather than an invented one.
			size_t first = a.size();
			if(pn == "Fusion.AddProcMixN")
				first = 3;
			else if(pn == "Fusion.AddProcMix" || pn == "Fusion.AddProcMixRep")
				first = 3;
			// CARDINAL EXTRACTION. Synchro/Xyz/Link carry no pairs: they carry
			// material COUNTS at fixed positions, read from the signature of the
			// contemporary proc_*.lua:
			//   Synchro.AddProcedure(c, f1,min1,max1, f2,min2,max2, ...) -> min1+min2
			//   Xyz.AddProcedure(c, f, lv, ct, alterf, desc, maxct, ...) -> ct
			//   Link.AddProcedure(c, f, min, max, ...)                   -> min
			// We extract ONLY the count, never the level or type we cannot read:
			// "N monsters" is WEAKER than the truth, so h stays admissible (the
			// under-constraint rule). When the procedure declares a range we take
			// the MINIMUM, same rule. Without this reading, every non-Fusion
			// extra deck monster is a place with NO PRODUCER: serialisation never
			// arms (Bagooska) and the extra deck vehicles stay outside A.
			auto mat_count = [&](size_t idx, uint64_t& out) {
				return idx < a.size() && kt.Eval(a[idx], out) &&
					   out >= 1 && out <= 15;
			};
			if(pn == "Xyz.AddProcedure") {
				uint64_t ct = 0;
				if(mat_count(3, ct))
					rc.unresolved_counts.push_back(static_cast<uint32_t>(ct));
			} else if(pn == "Link.AddProcedure") {
				uint64_t mn = 0;
				if(mat_count(2, mn))
					rc.unresolved_counts.push_back(static_cast<uint32_t>(mn));
			} else if(pn == "Synchro.AddProcedure") {
				uint64_t m1 = 0, m2 = 0;
				if(mat_count(2, m1))
					rc.unresolved_counts.push_back(static_cast<uint32_t>(
						mat_count(5, m2) ? m1 + m2 : m1));
			}
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
		// The whole "MustBe...Summoned" family: Fusion, Synchro, Xyz, Link,
		// Ritual. The script declares that the card only enters play through its
		// own procedure, and that is what forbids a "choose" conversion from
		// placing it (the state-of-the-art sink: a Liger in the graveyard never
		// comes back).
		if(line.find("AddMustBe") != std::string::npos &&
		   line.find("Summoned") != std::string::npos && !co.recipes.empty())
			co.recipes.back().must_be_fusion_summoned = true;

		// --- declared lists ---------------------------------------------------
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

		// --- zones and verbs touched by the current function ------------------
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
			// TYPES and RACES named by the function. The token-START guard is
			// mandatory: "EFFECT_TYPE_FIELD" contains "TYPE_" and so does
			// "SUMMON_TYPE_SYNCHRO"; without it, every SetType would pollute the
			// mask and the filter would become "anything at all".
			auto scan_mask = [&](const char* prefix,
								 std::unordered_map<std::string, uint64_t>& out) {
				const size_t plen = std::strlen(prefix);
				size_t tp2 = 0;
				while((tp2 = line.find(prefix, tp2)) != std::string::npos) {
					if(tp2 > 0 && IsIdentChar(line[tp2 - 1])) {
						tp2 += plen;
						continue;
					}
					size_t e2 = tp2;
					while(e2 < line.size() && IsIdentChar(line[e2]))
						++e2;
					uint64_t v = 0;
					if(kt.Lookup(line.substr(tp2, e2 - tp2), v))
						out[cur_fn] |= v;
					tp2 = e2;
				}
			};
			scan_mask("TYPE_", co.fn_types);
			scan_mask("RACE_", co.fn_races);
			// --- WHAT THE FUNCTION DESIGNATES: archetype and code -------------
			//
			// Same mechanism as the zones above, and it serves the TWO columns
			// the balance sheet was missing:
			//
			//   `IsSetCard(SET_X)` in a cost filter says WHICH archetype the
			//   destruction touches, "a Lunalight from the EXTRA" rather than
			//   "some card". Without it the negative column was typed by ZONE
			//   alone, hence unwritable without inventing it.
			//
			//   `IsCode(n)` says which exact code a recipe or a filter requires.
			//
			// We do not read the filter's semantics: we record the CONSTANTS it
			// names. A filter naming two archetypes yields two entries, and it is
			// for the caller to COUNT the ambiguity rather than resolve it.
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
			// THE FUNCTIONS THIS ONE CALLS. The constraint almost never lives in
			// the function that destroys: `descost` calls
			// `SelectMatchingCard(tp, s.descostfilter, ..., LOCATION_EXTRA, ...)`
			// and it is the FILTER that carries `IsSetCard(SET_LUNALIGHT)`.
			// Without that one-level hop, every negative edge stays at
			// "INDETERMINATE place"; both of this deck's were.
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

	// IN PROCEDURE MODE, an effect created in the target function counts EVEN
	// when it is not registered there: `Fusion.CreateSummonEff` BUILDS it and
	// RETURNS it, and `Fusion.RegisterSummonEff` registers it. Requiring
	// registration on the spot would lose exactly the operators we came for.
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

// The functions `fn` calls in its own body, of the form `NS.FN(`. ONE LEVEL is
// followed, deliberately: `Fusion.RegisterSummonEff` delegates to
// `Fusion.CreateSummonEff`, and two levels are enough to cover every game
// procedure. Following further would turn a reading into an interpretation.
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
	// End of block: the next top-level declaration.
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

// --- THE TABLE ---------------------------------------------------------------

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

	// The game's PROCEDURE files. They are scripts like any other, and that is the
	// only reason this list is acceptable: it names no card, only files of the rule
	// engine. A missing file is skipped, and the harness will say so by counting
	// the operators it cannot find, never by guessing.
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
		// OPERATORS DECLARED BY A PROCEDURE. A card that calls
		// `Pendulum.AddProcedure` has a "set the scale from the HAND" activation
		// written nowhere in its own script: it lives in `proc_pendulum.lua`.
		// Polymerization declares NOTHING but `Fusion.RegisterSummonEff(c)`.
		// Without this reading, the harness counts perfectly well declared
		// activations as failures.
		for(const std::string& pcall : co.proc_calls) {
			bool got = false;
			for(const auto& [fname, fsrc] : proc_src) {
				CardOperators pc = ParseScript(c, fsrc, kt, pcall);
				std::string via = pcall;
				if(pc.operators.empty()) {
					// ONE level of indirection: `RegisterSummonEff` delegates to
					// `CreateSummonEff`, which is the one that creates the effect.
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
				break;   // the procedure is declared in ONE file
			}
			(void)got;
		}
		cards[c] = std::move(co);
	}
	// Index of the descriptions. A collision (two cards declaring the same value)
	// is possible in theory and harmless here: the value CARRIES the card's code,
	// so two identical entries designate the same card.
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
	// An effect whose description is set by a clone: we fall back on the variable
	// name, the only other available anchor.
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
			return nullptr;   // ambiguous: we do not guess
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
			// The declared CONSUMPTION: the zones the cost touches.
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

// CLASSIFYING THE VERBS, and the nuance that decides everything.
//
// `Duel.SendtoHand` on an EXTRA monster does not go to the hand: the core puts
// it back in the EXTRA, which is why the scripts test
// `IsLocation(LOCATION_HAND|LOCATION_EXTRA)` right after the call. So it is a
// RECYCLER, a POSITIVE entry on the place aimed at, not a consumption. Same for
// `SendtoDeck`. That is the objection that forbids reasoning in STOCKS: the
// model is a model of FLOWS.
//
// THE ASYMMETRY OF THE RISK dictates the classification, and it is not
// symmetric at all: forgetting a CONSUMPTION makes the balance sheet too
// optimistic, i.e. looser, never wrong. Forgetting a PRODUCTION makes it
// UNSOUND, and it would kill a real line. So when in doubt a verb goes into
// "recycles" or "other", NEVER into "destroys".
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

// THE TWO NAMING CONVENTIONS, AND THEY MUST BE REDUCED TO ONE.
//
// `cur_fn` (the key of `fn_verbs`) is set at the DEFINITION and strips the
// prefix: `function s.spop(...)` -> "spop". The `fn_*` fields of an effect are
// set at the USE site and keep it: `e2:SetOperation(s.spop)` -> "s.spop".
// Comparing the two as they stand NEVER matches, hence never yields a capacity,
// and a consumer with no known bound is exactly the case where the balance
// sheet says nothing: a live report printing pessimism everywhere.
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

// The declared capacity of the operator `fn` is one of the functions of.
// Returns null when no effect carries it, and the caller then prints
// "UNBOUNDED": a pessimistic reading for a consumer, an optimistic one for a
// recycler. We print it rather than assume it.
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
	// One row of the material balance: (card, function) -> classified verbs, zones
	// touched, declared capacity.
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
			s += x.substr(5);   // without the "Duel." prefix
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

	// THE SYNTHESIS, AND IT IS THE JUDGE. We keep only the edges that consume IN
	// the goal's zone: that is the `A_p` row of the balance equation, the one
	// whose feasibility decides that a rollout is dead.
	const std::string zname = kt.MaskNames("LOCATION_", goal_zone);
	std::printf("\n  --- CE QUI CONSOMME DANS %s (la ligne du but) ---\n",
				zname.empty() ? "<zone>" : zname.c_str());
	size_t n_destroy = 0, n_recycle = 0;
	for(const Edge& e : edges) {
		if(!(e.locs & goal_zone))
			continue;
		// CAPACITY IS THE DECIDING FIGURE, on both sides of the sign: on a consumer
		// it bounds how many tokens can leave, on a recycler how many can come back.
		// "per NAME" is 1 whatever the number of copies; "per COPY" is worth as many
		// as there are placeable copies. It is the `Y_o <= cap` bound of the
		// operator-counting framework, and without it the balance sheet tightens
		// nothing.
		char cap[48];
		if(e.has_cap)
			std::snprintf(cap, sizeof cap, "[cap %u/tour par %s]", e.cap,
						  e.cap_by_name ? "NOM" : "COPIE");
		else
			std::snprintf(cap, sizeof cap, "[SANS BORNE declaree]");
		if(!e.destroy.empty()) {
			++n_destroy;
			// THE PLACE, and no longer just the zone. `IsSetCard`/`IsCode` in the
			// filter say WHAT the destruction touches; without them the negative
			// edge could not be attributed to any place.
			std::string what;
			auto cit = cards.find(e.code);
			if(cit != cards.end()) {
				// The function, THEN the functions it calls (the filter).
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

// --- THE MISSING NODE TYPE ---------------------------------------------------

std::vector<AcquirableCode> AcquirableCodesOf(const OperatorTable& tbl,
											  const CardDB& db,
											  const std::vector<uint32_t>& owned) {
	std::vector<AcquirableCode> out;
	// THE ONLY CODES WORTH ACQUIRING: those a DECLARED recipe names as a MATERIAL.
	//
	// THE FIRST VERSION POSTED THIRTEEN EDGES, AND TWELVE WERE HARMFUL. "Kaleido
	// Chick can acquire Liger Dancer's code" is true in the game's sense, and to
	// borrow it you must send a Liger from the EXTRA to the graveyard. The deck
	// has three, the goal asks for three: the edge proposes a route that DESTROYS
	// the goal. And it is useless, since no recipe requires "Liger Dancer" as a
	// material.
	//
	// On benchmark A, only one recipe names anything at all (Liger requires
	// 24550676): thirteen edges become ONE. That also explains the graph's average
	// distance going from 14 to 107, twelve useless edges in an already cyclic
	// graph.
	//
	// THIS IS NOT PRUNING OF THE ACTION SPACE: nothing is taken away from the
	// solver. We remove edges from a HEURISTIC, i.e. routes the graph described as
	// useful and that are not.
	//
	// ACKNOWLEDGED LIMIT: only DECLARED recipes (`Fusion.AddProcMix*`) count here.
	// A named requirement coming only from card TEXT therefore opens no
	// acquisition; under-estimating is the safe direction, and text seeding stays
	// posted on its own side.
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
			// THE OPERATOR THAT SETS THIS STATE. The link is the function name:
			// the state is created in `s.operation`, and the operator declares
			// `SetOperation(s.operation)`. No other anchor exists, and it is the
			// one the scripts write.
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
			// THE ZONES THE OPERATOR REACHES, read from its functions. For
			// `Kaleido Chick`, the cost sweeps `LOCATION_DECK|LOCATION_EXTRA`,
			// and that is where the code comes from.
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
			// THE REACHABLE CODES. `s.listed_series` is the card's own
			// DECLARATION of the archetype it manipulates: using it is not
			// tuning, it is reading what the script announces to the engine.
			// With no declaration we do not restrict; under-estimating is the
			// safe direction, inventing a filter is not.
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
				// ACQUIRING ONE'S OWN NAME PRODUCES NOTHING. The edge would be a
				// self-loop in an already cyclic graph, and the distance would pay
				// for it at every level of recursion.
				if(code == c)
					continue;
				// NO RECIPE ASKS FOR THIS CODE: the acquisition is useless, and its
				// cost consumes the reserve. See the header.
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

// --- THE HARNESS -------------------------------------------------------------

HarnessVerdict ConfrontPlan(const OperatorTable& tbl, const CardDB& db,
							const ConstantTable& kt,
							const std::vector<ObservedActivation>& acts) {
	HarnessVerdict v;
	const uint64_t act_mask = OperatorTable::ActivatableMask(kt);
	// Resource counter: (turn, card, chain index) -> uses.
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
		bool zone_is_evidence = false;   // the zone served to DESIGNATE the operator
		bool exact = true;               // is the operator DETERMINED?
		if(ds) {
			++v.matched_by_desc;
			de = tbl.EffectOf(*ds);
		} else if(co) {
			// The description is not an `aux.Stringid` of a card in the deck.
			// Two cases, and they do not say the same thing.
			//
			// (1) THE OPERATOR DECLARES THIS SYSTEM STRING ITSELF. That is the
			//     case of the procedures: `Pendulum.AddProcedure` sets
			//     `SetDescription(1160)` ("activate as a Pendulum Spell").
			//     The match stays EXACT.
			for(const DeclaredEffect& e : co->operators)
				if(e.desc_is_system && e.desc_value == a.desc) {
					de = &e;
					break;
				}
			if(de) {
				++v.matched_by_system;
			} else {
				// (2) THE DESCRIPTION COMES FROM THE CORE. `processor.cpp:743` emits
				//     221 and `:443` emits 0 for "activate this card's trigger
				//     effect?": the message carries the CARD and not the effect. No
				//     extraction can do better; it is the protocol that does not
				//     carry the information. So we match on the card, breaking ties
				//     by ZONE when that is enough, and we COUNT what stays
				//     ambiguous.
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
					// THE OPERATOR IS NOT DETERMINED. Judging a precondition on an
					// arbitrarily chosen operator would return MANUFACTURED
					// discrepancies: `Lunalight Gold Leo` has three trigger effects,
					// two of which share a counter while the third has another
					// (`SetCountLimit(1,{id,1})`). Taking the first and counting its
					// uses raises a "resource exceeded" that does not exist.
					exact = false;
				}
				de = cand.front();
			}
		}
		if(!de)
			continue;   // matched to a RESOLUTION string: no zone to judge
		if(!exact)
			continue;   // nothing checkable: the operator is not designated
		// A zone that served to DESIGNATE the operator cannot then judge it: the
		// check would be vacuous by construction.
		// ZONE PRECONDITION. It is only checkable on the prompts that carry the
		// activation zone (IDLECMD, CHAIN) and for an effect that declares a
		// range.
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
		// RESOURCE PRECONDITION. `SetCountLimit(n, id)` counts per PLAYER and per
		// (code, hopt_index), checked against `field::get_count_map`
		// (`field.cpp:1452`, key `code << 32 | hopt_index << 16 | flag << 8 |
		// playerid`) rather than assumed: so it really is "n times per turn and
		// per NAME", and two effects of one card share the counter UNLESS they
		// carry different indices (`SetCountLimit(1,{id,1})`). The key is
		// therefore the TAG written in the script, never the description index;
		// two effects can share the description and not the counter, and the
		// other way round.
		//
		// The variant WITHOUT a tag (`SetCountLimit(1)`) is "per copy": it would
		// require tracking a card's physical identity across its moves, and so is
		// not checkable from the messages.
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

// --- THE SIMPLEX -------------------------------------------------------------
//
// Two phases, BLAND's rule. Bland is slower than the most-negative-cost rule,
// and that is exactly why it is used: it **guarantees termination** (no
// cycling), and our instances are tiny. A solver looping on a degenerate
// instance would be undetectable inside a search run; a slow solver is not.
//
// Solved form, after normalisation:   min c'x   s.t.  Ex = f,  x >= 0
//   - constraint `a.x >= b` with b >= 0 -> a.x - surplus = b, + artificial
//   - constraint `a.x >= b` with b <  0 -> (-a).x + slack = -b   (basis given)
//   - bound `x_j <= u_j`                -> x_j + slack = u_j     (basis given)
namespace {

constexpr double kEps = 1e-9;

struct Tableau {
	size_t m = 0, n = 0;                 // rows, columns (excluding the right-hand side)
	std::vector<double> a;               // m x (n+1), right-hand side last
	std::vector<size_t> basis;           // basic variable per row
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

// Returns false when the program is unbounded (impossible here: x is bounded or
// the cost is positive, but we do not ASSUME it).
//
// `enterable`: only columns j < enterable may ENTER the basis. Artificials are
// excluded in BOTH phases, which is the standard result (Chvatal) replacing the
// old 1e12 phase-2 cost: the feasible point (x*, art = 0) belongs to the
// restricted polyhedron, so the restricted optimum equals the full optimum and
// an artificial can never re-enter. The 1e12 cost left a re-entry possible when
// a degenerate basis carried a coefficient > 1 on the artificial column.
bool Optimize(Tableau& t, std::vector<double>& cost, std::vector<double>& red,
			  size_t enterable) {
	for(;;) {
		// Reduced costs: c_j - c_B B^-1 A_j, computed by subtracting the basis
		// rows (the tableau is already in canonical form).
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
		// BLAND: first column (smallest index) with a negative reduced cost.
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
			// BLAND on the way out too: at equal ratio, smallest basis index.
			// It is that pair which forbids cycling.
			if(leave == t.m || ratio < best - kEps ||
			   (ratio < best + kEps && t.basis[i] < t.basis[leave])) {
				leave = i;
				best = ratio;
			}
		}
		if(leave == t.m)
			return false;   // unbounded
		Pivot(t, leave, enter);
	}
}

}   // namespace

LPResult SolveOperatorLP(const OperatorLP& lp) {
	LPResult res;
	const size_t N = lp.n_ops;
	// Counting the columns: x, then one surplus/slack per constraint, then one
	// slack per finite bound, then the artificials.
	std::vector<size_t> bound_of;   // indices of operators with a finite bound
	for(size_t j = 0; j < N; ++j)
		if(lp.upper[j] < OperatorLP::kNoBound * 0.5)
			bound_of.push_back(j);
	const size_t R = lp.rows.size(), B = bound_of.size();
	// A right-hand side in (0, kEps] would be treated as <= 0 and would produce a
	// SLIGHTLY infeasible starting basis (negative basic slack). The model's
	// right-hand sides are integers; we crush the noise before sorting.
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
		t.At(i, N + i) = needs_art[i] ? -1.0 : 1.0;   // surplus or slack
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
	// PHASE 1: minimise the sum of the artificials.
	if(n_art) {
		for(size_t j = N + R + B; j < t.n; ++j)
			cost[j] = 1.0;
		if(!Optimize(t, cost, red, N + R + B))
			return res;
		double inf = 0.0;
		for(size_t i = 0; i < t.m; ++i)
			if(t.basis[i] >= N + R + B)
				inf += t.At(i, t.n);
		if(inf > 1e-6) {
			// THE DIAGNOSIS OF INFEASIBILITY. The artificial left positive
			// belongs to ONE original row: that row is what cannot be satisfied
			// from this state. Naming it turns "h = INFINITE" into a work list,
			// and that is the condition for dead-end pruning to be auditable.
			// Row i's artificial is the (Sigma needs_art[0..i])-th.
			std::vector<size_t> art_row;
			for(size_t i = 0; i < R; ++i)
				if(needs_art[i])
					art_row.push_back(i);
			for(size_t i = 0; i < t.m; ++i)
				if(t.basis[i] >= N + R + B && t.At(i, t.n) > 1e-6) {
					const size_t k = t.basis[i] - (N + R + B);
					if(k < art_row.size())
						res.infeasible_rows.push_back(
							lp.rows[art_row[k]].label);
				}
			return res;   // INFEASIBLE: theorem 3, dead end PROVED
		}
		// Drive residual artificials out of the basis (pivot on any non-zero
		// non-artificial column); otherwise phase 2 could reintroduce them.
		// reintroduire.
		for(size_t i = 0; i < t.m; ++i) {
			if(t.basis[i] < N + R + B)
				continue;
			for(size_t j = 0; j < N + R + B; ++j)
				if(std::fabs(t.At(i, j)) > kEps) { Pivot(t, i, j); break; }
		}
	}
	// PHASE 2: the real cost. Artificials are barred from returning by
	// `enterable`, and those still in the basis after the drive-out have a zero row
	// on every non-artificial column (otherwise the drive-out would have pivoted),
	// so their value stays at zero whatever happens: no punitive cost is needed.
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
	// ROUNDING UP IS PART OF THE STATEMENT, NOT AN EMBELLISHMENT.
	// `h = ceil(c'x*)` (Bonet, def. and th. 2): costs are integers, so the cost of
	// any plan is an integer and is bounded below by `c'x*`; rounding up therefore
	// stays <= h*, and tightens for free. The solver returns FRACTIONAL `x` (the
	// continuous relaxation of the integer problem); measured on benchmark A:
	// `x1.5 rename`. Without the rounding, `h` would announce a value no plan can
	// achieve.
	// `+ 0.0`: ceil(0 - 1e-9) returns -0.0, which prints as "-0"; the addition
	// normalises it to +0.0 (IEEE, round to nearest).
	res.value = std::ceil(res.value - 1e-9) + 0.0;

	// THE DUALS, READ FROM THE FINAL TABLEAU. A row's dual is the reduced cost of
	// its surplus/slack (the sign of the normalisation and that of the negation of
	// rows with a negative right-hand side cancel out: y_i = red[N+i] uniformly,
	// >= 0 at the optimum). A bound's dual is the reduced cost of the bound slack:
	// the value of ONE extra unit of capacity.
	res.row_dual.assign(R, 0.0);
	for(size_t i = 0; i < R; ++i)
		res.row_dual[i] = red[N + i];
	res.bound_dual.assign(N, 0.0);
	for(size_t k = 0; k < B; ++k)
		res.bound_dual[bound_of[k]] = red[N + R + k];

	// --- THE GUARDS, AND THEY ARE THE REASON THIS BLOCK EXISTS ---------------
	// Theorems 1 to 4 only hold if THIS solver does not lie. So we check the
	// returned solution against the statement, on every call: primal feasibility
	// (A'x >= b, 0 <= x <= u) and optimality (no negative reduced cost left).
	res.primal_ok = true;
	res.worst_violation = 0.0;
	for(size_t j = 0; j < N; ++j) {
		// The violation is the EXCESS beyond the bound, not the absolute value of x:
		// |x| conflated "x = 3 for u = 2" (violation 1) and "x = 3 with no bound
		// reached" (violation 0) into the same number.
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
	{
		std::lock_guard<std::mutex> lock(sc_mx);
		auto it = sc_cache.find(code);
		if(it != sc_cache.end())
			return it->second;
	}
	std::vector<uint64_t> out;
	if(const CardRow* r = db->Find(code))
		for(uint16_t sc : r->setcodes)
			if(sc)
				out.push_back(sc & 0x0fffu);   // archetype, subtype ignored
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	std::lock_guard<std::mutex> lock(sc_mx);
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

// THE ALIAS, WHICH `Canonical` DOES NOT ALWAYS REDUCE. The core's
// `card::get_code()` resolves `alias` (card.cpp:236): 90590304 (Bagooska lying
// down) and 90590303 are the SAME card, and the board shows it. Comparing raw
// codes manufactures two places for one object, hence a goal with no producer.
static uint32_t AliasOf(const CardDB& db, uint32_t c) {
	const CardRow* r = db.Find(c);
	if(r && r->alias)
		return r->alias;
	return db.Canonical(c);
}

bool BalanceModel::Build(const OperatorTable& tbl, const CardDB& cdb,
						 const ConstantTable& kt,
						 const std::vector<uint32_t>& deck,
						 const std::vector<std::pair<uint32_t, uint32_t>>& goal,
						 const std::vector<std::pair<uint32_t, uint32_t>>& transient) {
	db = &cdb;
	// RESET: Build is called again on the SAME instance at every round of the
	// internal loop (`g_balance_model` is a process-wide static). Without this
	// reset, the second Build STACKED places, transitions and demands on top of
	// the first: the LP became infeasible ("the material balance yields no
	// plan"), serialisation disarmed, and reenter/refine-after/quota-h went
	// INERT on every round >= 2. Caught by the "!! INERT" line of the smoke
	// test; mechanism liveness is the only instrument that said so.
	pid.clear();
	pname.clear();
	tname.clear();
	thost.clear();
	need.clear();
	col.clear();
	lp = OperatorLP{};
	goal_codes.clear();
	n_rename = 0;
	n_igniter = 0;
	{
		std::lock_guard<std::mutex> lk(sc_mx);
		sc_cache.clear();
	}
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
		else if(kind == 3 && key >= (1ull << 40))
			// The Fusion IGNITION place: every summon through a
			// Fusion.AddProcMix* recipe consumes one; the operators with
			// CATEGORY_FUSION_SUMMON produce them, at their declared capacity.
			std::snprintf(b, sizeof b, "ignitions de Fusion");
		else if(kind == 3)
			// Pool of CHOICES for a summon product: the effect drops one unit
			// there, and the conversions specialise it towards ONE card.
			std::snprintf(b, sizeof b, "choix de l'effet #%llu",
						  (unsigned long long)key);
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
	// The places exist from CONSTRUCTION on: the marking changes from one state to
	// the next, the structure does not. That is what makes it possible to solve the
	// same model along a line without rebuilding it at every decision.
	// GENERIC PLACE "a monster available" (kind 2, key 0).
	//
	// `Xyz.AddProcedure(c, nil, 4, 2)` = "2 Level 4 monsters": a CARDINAL
	// requirement naming neither card nor archetype, hence invisible to the first
	// two grains. Without it, Bagooska has NO producer and the whole program
	// becomes infeasible; that is what the "place with NO PRODUCER" guard named.
	//
	// The level itself is not carried by `DeclaredRecipe` (only the COUNT is), so
	// we require "N monsters", not "N level 4 monsters". That is WEAKER than the
	// truth, so `h` is smaller and admissibility holds. Requiring the level
	// without reading it would have broken it.
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
	auto add_tr = [&](const char* what, double cap, double cost = 1.0,
					  uint32_t host = 0) {
		lp.cost.push_back(cost);
		lp.upper.push_back(cap);
		tname.push_back(what);
		thost.push_back(host);
		return lp.n_ops++;
	};
	auto eff = [&](size_t t, size_t p, double v) {
		if(col.size() <= t)
			col.resize(t + 1);
		col[t][p] += v;
	};
	// (1) THE DECLARED EFFECTS, AND NOTHING ELSE.
	//
	// NO INVENTED TRANSITION. A `mobilise` transition -- "every card in the deck
	// is one action away from the available zone" -- buys admissibility with
	// optimism, and measures 5 % of descents against 15 % for novelty, i.e. a
	// gradient WORSE than the one it is meant to replace. Nor is the answer to
	// bound it with rules ("one Normal Summon per turn"): those rules do not bear
	// on it, and "a card only leaves the deck through an effect that names it" is
	// FALSE, since a search by level names nothing, so the constraint would forbid
	// real plans and break admissibility.
	//
	// ONLY THE CARDS' REAL EFFECTS COUNT. Each `Duel.SetOperationInfo` declares a
	// (category, source zone) pair: that is a transition, with its origin zone, its
	// destination read from the category, and the capacity of the effect carrying
	// it. The place comes from the filter (`IsSetCard`/`IsCode`, via `fn_refs`),
	// else from the card's `listed_series`/`listed_names`, else from the card's own
	// archetype. None of those sources is a rule we add: all are declared in the
	// script.
	//
	// WHAT THAT CHANGES FOR THEOREM 1. `h` becomes admissible RELATIVE TO THE
	// DECLARED MODEL: if an effect escaped extraction, a real plan could violate a
	// row. That is not idle speculation, it is exactly what the harness MEASURES
	// (44 activations, 0 unmatched on `liger.yrpX`). So the guarantee is
	// conditioned on a number re-read on every run, not on a belief.
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
			// Destination, read from the CATEGORY.
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
				continue;   // a category that MOVES nothing: not a transition
			// Source, read from the DECLARED zone. `0` = not declared: we take
			// the RESERVE, the most useful reading and hence the most
			// optimistic, hence the one that cannot make `h` too large.
			const int src = (pr.location == 0 || (pr.location & 0x41u))
								? kRes
								: kAva;
			if(src == dst && !also_field)
				continue;   // moves nothing among our three zones
			// The PLACE: the filter first, the card second. Never an
			// assumption.
			std::vector<uint64_t> arch;
			std::vector<uint32_t> named;
			std::vector<std::string> scope{ pr.in_function };
			auto rit = co.fn_refs.find(pr.in_function);
			if(rit != co.fn_refs.end())
				for(const std::string& r : rit->second)
					scope.push_back(r);
			uint64_t type_mask = 0, race_mask = 0;
			for(const std::string& f : scope) {
				auto sit = co.fn_setcodes.find(f);
				if(sit != co.fn_setcodes.end())
					for(uint64_t v : sit->second)
						arch.push_back(v & 0x0fffull);
				auto cit2 = co.fn_codes.find(f);
				if(cit2 != co.fn_codes.end())
					for(uint32_t v : cit2->second)
						named.push_back(cdb.Canonical(v));
				auto tit = co.fn_types.find(f);
				if(tit != co.fn_types.end())
					type_mask |= tit->second;
				auto rit2 = co.fn_races.find(f);
				if(rit2 != co.fn_races.end())
					race_mask |= rit2->second;
			}
			if(arch.empty() && named.empty()) {
				for(uint64_t v : co.listed_series)
					arch.push_back(v & 0x0fffull);
				for(uint32_t v : co.listed_names)
					named.push_back(cdb.Canonical(v));
			}
			if(arch.empty() && named.empty() && !type_mask && !race_mask)
				for(uint64_t v : SetcodesOf(host))
					arch.push_back(v);
			// A product that puts NOTHING into play and whose filter names
			// neither code nor archetype stays outside the model (nothing to
			// place). A SUMMON product is kept as soon as a vocabulary is
			// readable, and failing any vocabulary it stays "a monster from the
			// deck" (under-constraint: we WIDEN, we do not guess by narrowing).
			if(!also_field && arch.empty() && named.empty())
				continue;
			// Capacity: that of the effect carrying this function, multiplied
			// by the copies when it is "per COPY".
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
			const size_t t = add_tr("effet", cap, 1.0, host);
			if(!also_field) {
				for(uint32_t nc : named) {
					eff(t, place(0, nc, src), -1.0);
					eff(t, place(0, nc, dst), +1.0);
				}
				if(named.empty())
					for(uint64_t a : arch) {
						eff(t, place(1, a, src), -1.0);
						eff(t, place(1, a, dst), +1.0);
					}
				continue;
			}
			// CHOOSING THE PRODUCT. A SUMMON product puts ONE PRECISE card into
			// play; modelling it "by archetype" left the card a reviver puts back
			// into play with NO producer (measured: 22 states with h = INFINITE in
			// the middle of benchmark B's real line, named "Abyss @FIELD" by the
			// phase 1 diagnosis, because the reviver filters by RACE/TYPE, invisible
			// to codes and archetypes).
			//
			// Shape: the effect drops ONE unit into a pool (its capacity is
			// PRESERVED, the conversions share the pool), and a ZERO-cost conversion
			// (it is a choice, not one more action; admissibility requires that)
			// specialises it towards each candidate. Candidates: the named codes PLUS
			// the deck monsters carrying the archetype or the TYPE/RACE the filter
			// names. It is a UNION, never a restriction: a code in range may be cited
			// in NEGATION (`not IsCode(...)`, Crimson Dragon), and restricting on it
			// would manufacture the over-constraint we are fixing. Failing any
			// vocabulary: every monster in the deck.
			if(named.empty())
				for(uint64_t a : arch) {
					eff(t, place(1, a, src), -1.0);
					eff(t, place(1, a, dst), +1.0);
				}
			std::vector<uint32_t> cand = named;
			for(const auto& cx : copies) {
				const CardRow* row = cdb.Find(cx.first);
				if(!row || !(row->type & 0x1u))
					continue;   // a summon product is a MONSTER
				bool okc = false;
				for(uint64_t a : arch) {
					for(uint64_t sc : SetcodesOf(cx.first))
						okc = okc || sc == (a & 0x0fffull);
				}
				if(!okc && (type_mask || race_mask))
					okc = (type_mask && (row->type & type_mask) != 0) ||
						  (race_mask && (row->race & race_mask) != 0);
				if(!okc && arch.empty() && named.empty() &&
				   !type_mask && !race_mask)
					okc = true;   // unreadable filter: every monster in the deck
				// NEVER a "MustBe...Summoned" through a conversion. The card's
				// script declares that it only enters play through its own summon
				// procedure: a reviver cannot choose it. Without that exclusion,
				// declared and not chosen, archetype revivers placed Liger at cost 1
				// and benchmark A's h(start) fell from 14 to 3: the whole model went
				// soft.
				if(okc)
					if(const CardOperators* co2 = tbl.Find(cx.first))
						for(const DeclaredRecipe& r2 : co2->recipes)
							if(r2.must_be_fusion_summoned)
								okc = false;
				if(okc)
					cand.push_back(cx.first);
			}
			std::sort(cand.begin(), cand.end());
			cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
			if(cand.empty())
				continue;
			const size_t pool = place(3, t, 0);
			eff(t, pool, +1.0);
			for(uint32_t c2 : cand) {
				const size_t tc =
					add_tr("choisir", OperatorLP::kNoBound, 0.0, host);
				eff(tc, pool, -1.0);
				eff(tc, place(0, c2, src), -1.0);
				eff(tc, place(0, c2, dst), +1.0);
				eff(tc, place(0, c2, kFld), +1.0);
			}
		}
	}
	// IGNITION COUPLING. The LP used to "fuse directly": the recipe summoned
	// with no igniter, so Wolf's quota (SetCountLimit(1), PZONE) was INVISIBLE
	// to the relaxed plan and the duals could not designate it. Every summon
	// through a `Fusion.AddProcMix*` recipe now consumes ONE ignition; the
	// operators declared with CATEGORY_FUSION_SUMMON produce them, at ZERO cost
	// (ignition and summon are ONE action; admissibility requires it) and at
	// their DECLARED capacity. Asymmetry guard: if NO igniter is readable while
	// Fusion recipes exist, we do NOT couple, since an incomplete production
	// column would turn an extraction gap into a false proof of a dead end.
	uint64_t c_fusion_summon = 0;
	kt.Lookup("CATEGORY_FUSION_SUMMON", c_fusion_summon);
	struct Igniter { uint32_t host; const DeclaredEffect* e; };
	std::vector<Igniter> igniters;
	if(c_fusion_summon)
		for(const auto& kv : tbl.All()) {
			const uint32_t host = AliasOf(cdb, kv.first);
			if(!copies.count(host))
				continue;   // an igniter outside the deck does not exist
			for(const DeclaredEffect& e : kv.second.operators)
				if(e.at_init && (e.category & c_fusion_summon))
					igniters.push_back({ host, &e });
		}
	n_igniter = igniters.size();
	const size_t p_ign = igniters.empty() ? size_t(-1)
										  : place(3, 1ull << 40, 0);
	// (2) SUMMONING: the DECLARED recipe, with its MULTIPLICITY.
	for(const auto& kv : tbl.All()) {
		const uint32_t c = kv.first;
		const CardOperators& co = kv.second;
		if(co.recipes.empty())
			continue;
		// THE FIRST NON-EMPTY RECIPE, not front(): a Pendulum card calls
		// `Pendulum.AddProcedure` before its summon procedure, and that recipe is
		// empty; it would mask the real one.
		// `unresolved_counts` COUNTS: "2 Level 4 monsters" is a complete recipe.
		// Leaving it out of the guard deprived Bagooska, and every Xyz, of a
		// producer, hence made the whole program infeasible.
		const DeclaredRecipe* rp = nullptr;
		for(const DeclaredRecipe& cand : co.recipes)
			if(!cand.named.empty() || !cand.setcode.empty() ||
			   !cand.unresolved_counts.empty()) {
				rp = &cand;
				break;
			}
		if(!rp)
			continue;
		const DeclaredRecipe& r = *rp;
		// THE PRODUCT IS CANONICALISED, like the goal. `--target 90590304` names
		// Bagooska's lying-down form; the table indexes `90590303`. Without that
		// reduction the two places are distinct, the recipe produces nothing for
		// the goal, and the whole program becomes infeasible.
		const uint32_t cc = AliasOf(cdb, c);
		const size_t t = add_tr("invoquer", OperatorLP::kNoBound, 1.0, cc);
		eff(t, place(0, cc, kRes), -1.0);
		eff(t, place(0, cc, kFld), +1.0);
		eff(t, place(0, cc, kAva), +1.0);
		if(p_ign != size_t(-1) && r.proc.rfind("Fusion.AddProcMix", 0) == 0)
			eff(t, p_ign, -1.0);
		for(const auto& m : r.named)
			eff(t, place(0, cdb.Canonical(m.first), kAva),
				-static_cast<double>(m.second));
		for(const auto& sc : r.setcode)
			eff(t, place(1, sc.first & 0x0fffull, kAva),
				-static_cast<double>(sc.second));
		for(uint32_t k : r.unresolved_counts)
			eff(t, place(2, 0, kAva), -static_cast<double>(k));
	}
	// (3) RENAMING: the parameterised family of EFFECT_ADD_CODE. The granted code
	// is `e:GetLabel()`, set at COST time from the card sent to the graveyard,
	// hence +2 in AVAILABLE: the renamed bearer counts as X for a Fusion, AND the
	// card sent there is made a material by EFFECT_EXTRA_FUSION_MATERIAL.
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
			const size_t t = add_tr("renommer", cap, 1.0, host);
			eff(t, place(0, x, kRes), -1.0);
			eff(t, place(0, x, kAva), +2.0);
			++n_rename;
		}
	}
	// (4) THE FUSION IGNITERS, built LAST: the capacity of an igniter with no
	// SetCountLimit derives from its copies, but ONLY if no already-built
	// transition can reproduce it (a recyclable spell has no provable bound, per
	// the under-constraint rule).
	if(p_ign != size_t(-1)) {
		for(const Igniter& ig : igniters) {
			double cap;
			if(ig.e->has_count_limit) {
				cap = ig.e->count_by_name
						  ? double(ig.e->count_limit)
						  : double(ig.e->count_limit) *
								double(copies.at(ig.host));
			} else {
				cap = double(copies.at(ig.host));
				std::vector<size_t> host_places;
				for(int z : { kRes, kAva }) {
					size_t p = PlaceId(0, ig.host, z);
					if(p != size_t(-1))
						host_places.push_back(p);
					for(uint64_t sc : SetcodesOf(ig.host)) {
						p = PlaceId(1, sc, z);
						if(p != size_t(-1))
							host_places.push_back(p);
					}
				}
				for(size_t t2 = 0; t2 < col.size() && cap < OperatorLP::kNoBound;
					++t2)
					for(size_t hp : host_places) {
						auto itp = col[t2].find(hp);
						if(itp != col[t2].end() && itp->second > 0)
							cap = OperatorLP::kNoBound;   // reproducible
					}
			}
			const size_t t = add_tr("igniter", cap, 0.0, ig.host);
			eff(t, p_ign, +1.0);
		}
	}
	{
		const size_t gen = place(2, 0, kAva);
		for(size_t t = 0; t < col.size(); ++t) {
			// "Choose" conversions do NOT feed the generic place (the body
			// they specialise is already counted by the effect that filled the
			// pool), and an IGNITION is not a body.
			// corps (s22).
			if(tname[t] == "choisir" || tname[t] == "igniter")
				continue;
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
	// THE TRANSIENT DEMANDS: how resolutions are compiled into the balance sheet.
	// The junction diagnosis showed that all the credit for a required resolution
	// was POST-event (score, rung, key): no gradient existed BEFORE, and the card
	// to resolve only appeared in the terminal spasm of the lines (first summon at
	// decision 240 out of ~253 of life). Demanding its PRESENCE @AVAILABLE here
	// puts its manufacturing chain into x*, hence its places into the
	// serialisation, hence rungs MID-LINE, without a card name in the code: the
	// codes come from the flags, like the goal itself.
	//
	// @AVAILABLE and not @FIELD: AVAILABLE (hand, field, graveyard, BANISHED) is
	// cumulative along a line, since a body out of the reserve stays there, even
	// banished by its own resolution. It is the weakest "has existed" semantics,
	// hence admissible. Asymmetry guard: with no readable producer the demand is
	// not posted (it would make the WHOLE program infeasible over an extraction
	// gap), and it is named, like the igniter guard.
	for(const auto& g : transient) {
		const uint32_t cc = AliasOf(cdb, g.first);
		const size_t p = PlaceId(0, cc, kAva);
		bool producible = false;
		if(p != static_cast<size_t>(-1))
			for(size_t t = 0; t < col.size() && !producible; ++t) {
				auto it = col[t].find(p);
				producible = it != col[t].end() && it->second > 0;
			}
		if(!producible) {
			std::printf("  demande transitoire SANS PRODUCTEUR lisible : %s @DISPO "
						"— non posee (garde d'asymetrie : lacune d'extraction, "
						"pas une preuve)\n",
						cdb.Name(cc).c_str());
			continue;
		}
		need[p] += g.second;
		// The protection of goal codes applies here too: never a consumption
		// rung on a card to resolve (the Liger@GRAVEYARD poison applies to it
		// identically).
		goal_codes.push_back(cc);
	}
	col.resize(lp.n_ops);
	return true;
}

double BalanceModel::Solve(const std::vector<uint32_t>& res,
						   const std::vector<uint32_t>& ava,
						   const std::vector<uint32_t>& fld,
						   LPResult* out,
						   const std::vector<std::pair<uint32_t, uint32_t>>&
							   spent) const {
	std::vector<double> mark(pname.size(), 0.0);
	auto put = [&](const std::vector<uint32_t>& codes, int z) {
		for(uint32_t raw : codes) {
			const uint32_t c = db->Canonical(raw);
			size_t p = PlaceId(0, c, z);
			if(p != static_cast<size_t>(-1))
				mark[p] += 1.0;
			if(z == kFld)
				continue;   // archetypes are tracked in RESERVE and AVAILABLE only
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
	// THE PATH QUOTAS (red-black relaxation). Every host observed `s` times gets
	// ONE row aggregated over ALL its transitions: Sigma x_t <= (Sigma u_t) - s,
	// encoded as a >= row with -1 coefficients (the solver already accepts a
	// right-hand side and coefficients of any sign). The aggregate is safe under
	// any attribution of the activations to effects: Sigma s_t >= s, so the real
	// remaining budget is <= ours. We UNDER-constrain, h stays admissible. The two
	// asymmetry guards (effect with no bound; observation > budget) IGNORE the
	// host and NAME it in the result: a gap in the model must never become a proof
	// of death. The mandated judge is the theorem 2 walk WITH quotas along the
	// reference line: 0 new infeasible state is required before any use beyond
	// refinement.
	uint32_t quota_applied = 0;
	std::vector<uint32_t> quota_unbounded, quota_overrun;
	for(const auto& hs : spent) {
		if(!hs.second)
			continue;
		const uint32_t host = db->Canonical(hs.first);
		double budget = 0.0;
		bool unbounded = false;
		std::vector<size_t> ts;
		for(size_t t = 0; t < lp.n_ops && t < thost.size(); ++t) {
			if(thost[t] != host)
				continue;
			// THE AGGREGATE'S SET E: all the host's transitions EXCEPT those
			// that, BY CONSTRUCTION, never produce a MSG_CHAINING under its
			// code: `summon` (the body's own inherent summon, which is
			// MSG_SPSUMMONING and never a chain; an igniter that chains is the
			// host of ITS `ignite` transition, not of the summoned body's) and
			// `choose` (pool bookkeeping, no game action). Everything else,
			// `effect`, `rename`, `ignite`, STAYS in E: when in doubt we WIDEN
			// E, which widens the budget and WEAKENS the row (the safe
			// direction). Without that exclusion, every monster in the deck
			// carried its unbounded `summon` transition and the guard made the
			// mechanism inert everywhere (measured: bench B, 0 row posted, 2
			// hosts ignored as "unbounded").
			if(tname[t] == "invoquer" || tname[t] == "choisir")
				continue;
			ts.push_back(t);
			if(lp.upper[t] >= OperatorLP::kNoBound * 0.5)
				unbounded = true;
			else
				budget += lp.upper[t];
		}
		if(ts.empty() || unbounded) {
			quota_unbounded.push_back(host);
			continue;
		}
		if(static_cast<double>(hs.second) > budget + 1e-9) {
			quota_overrun.push_back(host);
			continue;
		}
		OperatorLP::Row row;
		for(size_t t : ts)
			row.coef.emplace_back(t, -1.0);
		row.rhs = -(budget - static_cast<double>(hs.second));
		row.label = "quota_chemin:" + db->Name(host);
		inst.rows.push_back(std::move(row));
		++quota_applied;
	}
	// TWO INFEASIBILITIES NOT TO BE CONFLATED, and conflating them would make
	// theorem 3 unusable.
	//
	//   "this goal is PROVED out of reach"     (copies, capacities: a fact)
	//   "I do not know how to make this place" (no transition produces it: a GAP
	//                                           in the model)
	//
	// The second is detectable before solving, and it costs one loop: a row with a
	// positive right-hand side and NO positive coefficient has no producer.
	// Announcing that as a proof would be a lie; announcing it as an extraction
	// gap turns it into a work list.
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
	r.quota_applied = quota_applied;
	r.quota_unbounded_hosts = std::move(quota_unbounded);
	r.quota_overrun_hosts = std::move(quota_overrun);
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
	// Aggregated by PRODUCED place: how many tokens each firing transition must
	// drop there. The rounding is UPWARD, since half a summon does not exist, and
	// under-counting a subgoal would make it half-crossable.
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
		if(kind == 3)
			continue;   // choice pools are not subgoals
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
	// MARKER: "what x* consumes lands in the GRAVEYARD" is a GAME hypothesis
	// (generally true in this game: materials and costs end up there by default),
	// NOT a universal rule; a cost that BANISHES or SHUFFLES INTO THE DECK escapes
	// it. The destination would be derivable from the cost's category when it is
	// declared; until that is done, the hypothesis stays WRITTEN here and the
	// third deck is its judge. The @BANISHED rungs were removed for measured
	// POISON (Silver); do not put them back without the plan's mandatory step.
	std::vector<Need> out;
	if(!r.feasible)
		return out;
	// Total consumption per place: Sigma_t x_t * (-negative coef).
	std::unordered_map<size_t, double> eaten;
	for(size_t t = 0; t < r.x.size() && t < col.size(); ++t) {
		if(r.x[t] <= 1e-6)
			continue;
		for(const auto& kv : col[t])
			if(kv.second < 0)
				eaten[kv.first] += -kv.second * r.x[t];
	}
	// Aggregated by IDENTITY (kind, key), zones conflated: a body consumed from the
	// RESERVE (renaming: deck -> graveyard) and a body consumed from AVAILABLE (a
	// fusion material) land in the same place. The generic place (kind 2) is left
	// out: "some monster in the graveyard" cannot be counted without double
	// counting against the named identities.
	std::unordered_map<uint64_t, double> merged;
	for(const auto& kv : pid) {
		auto it = eaten.find(kv.second);
		if(it == eaten.end())
			continue;
		if(static_cast<int>(kv.first >> 62) >= 2)
			continue;   // generic place, and choice pools
		merged[kv.first & ~3ull] += it->second;
	}
	for(const auto& kv : merged) {
		if(kv.second < 0.5)
			continue;
		Need n;
		const int kind = static_cast<int>(kv.first >> 62);
		const uint64_t k = (kv.first >> 2) & ((1ull << 60) - 1);
		// NEVER a consumption rung on a card OF THE GOAL. x* may consume a
		// Liger (destruction cost), but rewarding "Liger in the graveyard"
		// when the goal is "3 Liger" and the deck only has 3 manufactures
		// cells in APPARENT progress that have destroyed the goal. It is
		// measured: the real line never serves that rung, the bare run's
		// rollouts served it massively, and the tournament preferred those
		// poisoned cells.
		if(kind == 0 &&
		   std::find(goal_codes.begin(), goal_codes.end(),
					 static_cast<uint32_t>(k)) != goal_codes.end())
			continue;
		if(kind == 0)
			n.code = static_cast<uint32_t>(k);
		else
			n.arch = k;
		n.zone = 3;   // GRAVEYARD
		// Cap at 15: the packing of SerialProgress carries 4 bits per
		// requirement, and a rung beyond the cap would not exist.
		n.count = (std::min)(
			static_cast<uint32_t>(std::ceil(kv.second - 1e-9)), 15u);
		out.push_back(n);
		// THE @BANISHED RUNGS WERE REMOVED, on two measurements. (1) On the
		// bench they brought NO rung in the deserts: fusion banishments come
		// in a burst AT the summon, the same answer as the Liger
		// @AVAILABLE/@FIELD rungs already served. (2) On a run they were a
		// massive POISON CHANNEL: Silver Hound's negation (QUICK from the
		// graveyard) banishes as a COST Silver plus a Lunalight Fusion from
		// the graveyard, i.e. the Leo material. Leo reached the banished
		// zone in 25 884 rollouts for 260 Liger summons (98.9 % of suicides
		// rewarded by the ladder). The CHOICE of Silver stays playable; only
		// the reward is removed, the same gesture as Liger@GRAVEYARD.
	}
	return out;
}

std::vector<uint32_t> BalanceModel::QuotaHostsFrom(
	const LPResult& r, std::vector<uint32_t>* presence) const {
	std::vector<uint32_t> out;
	if(!r.feasible)
		return out;
	// The places the relaxed plan CONSUMES (coef < 0 of a fired transition): any
	// BOUNDED producer of one of them is a relevant quota, even if the simplex
	// routed through a zero-cost twin.
	std::vector<char> consumed(pname.size(), 0);
	for(size_t t = 0; t < lp.n_ops && t < r.x.size(); ++t) {
		if(r.x[t] <= 1e-6)
			continue;
		for(const auto& kv : col[t])
			if(kv.second < 0)
				consumed[kv.first] = 1;
		// Never a card OF THE GOAL: its quota is the plan's product, not a
		// resource, and its presence is already a @FIELD subgoal.
		if(presence && t < thost.size() && thost[t] &&
		   std::find(goal_codes.begin(), goal_codes.end(), thost[t]) ==
			   goal_codes.end())
			presence->push_back(thost[t]);
	}
	for(size_t t = 0; t < lp.n_ops; ++t) {
		const uint32_t host = t < thost.size() ? thost[t] : 0;
		if(!host)
			continue;
		if(std::find(goal_codes.begin(), goal_codes.end(), host) !=
		   goal_codes.end())
			continue;
		if(lp.upper[t] >= OperatorLP::kNoBound * 0.5)
			continue;   // no declared bound: not a quota
		const bool pulled = t < r.x.size() && r.x[t] > 1e-6;
		const bool saturated =
			t < r.x.size() && r.x[t] >= lp.upper[t] - 1e-6;
		const bool priced =
			t < r.bound_dual.size() && r.bound_dual[t] > 1e-6;
		bool feeds = false;
		if(!pulled && !saturated && !priced)
			for(const auto& kv : col[t])
				feeds = feeds || (kv.second > 0 && consumed[kv.first]);
		if(pulled || saturated || priced || feeds)
			out.push_back(host);
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	if(presence) {
		std::sort(presence->begin(), presence->end());
		presence->erase(std::unique(presence->begin(), presence->end()),
						presence->end());
	}
	return out;
}

double BuildAndSolveBalance(
	const OperatorTable& tbl, const CardDB& db, const ConstantTable& kt,
	const std::vector<uint32_t>& deck,
	const std::vector<std::pair<uint32_t, uint32_t>>& goal,
	const std::vector<std::pair<uint32_t, uint32_t>>& transient) {
	std::printf("\n=== LE BILAN MATIERE : h(depart) ===\n");
	BalanceModel m;
	if(!m.Build(tbl, db, kt, deck, goal, transient)) {
		std::printf("  (aucun but donne)\n");
		return -1.0;
	}
	std::printf("  places %zu, transitions %zu (dont %zu renommages)\n",
				m.Places(), m.Transitions(), m.Renames());
	LPResult r;
	// Starting state: the whole deck is in RESERVE. That is the state BEFORE the
	// draw, the one answering "can this deck, in principle?".
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
	// THE GENERALITY PROBE: the quotas and enablers DERIVED, on any deck, by the
	// same computation as the run. It is the judge of the derivation ("the same
	// command yields a finite h, subgoals, quotas").
	{
		std::vector<uint32_t> presence;
		const std::vector<uint32_t> qh = m.QuotaHostsFrom(r, &presence);
		std::string s1, s2;
		for(uint32_t c : qh)
			s1 += db.Name(c) + " ; ";
		for(uint32_t c : presence)
			s2 += db.Name(c) + " ; ";
		std::printf("  QUOTAS derives (duaux) : %s\n",
					s1.empty() ? "(aucun)" : s1.c_str());
		std::printf("  HABILITANTS tires par x* : %s\n",
					s2.empty() ? "(aucun)" : s2.c_str());
		if(m.FusionIgniters())
			std::printf("  igniteurs de Fusion lus : %zu (couplage d'ignition "
						"ARME)\n", m.FusionIgniters());
	}
	return h;
}

size_t SelfTestOperatorLP(size_t* total) {
	// FIVE INSTANCES WITH HAND-KNOWN SOLUTIONS. They cover exactly the four
	// theorems: multiplicity (the goal has 3 copies), consumption (a transition
	// that destroys), capacity (theorem 4) and infeasibility (theorem 3). A solver
	// that passes them all is not proved right; a solver that fails one is proved
	// wrong, and that is what we want.
	struct Case { OperatorLP lp; bool feas; double val; };
	std::vector<Case> cs;
	auto mk = [](size_t n) {
		OperatorLP lp;
		lp.n_ops = n;
		lp.cost.assign(n, 1.0);
		lp.upper.assign(n, OperatorLP::kNoBound);
		return lp;
	};
	{   // 1. a single operator produces the goal: x >= 3  ->  h = 3
		Case c; c.lp = mk(1);
		c.lp.rows.push_back({ { { 0, 1.0 } }, 3.0, "but" });
		c.feas = true; c.val = 3.0; cs.push_back(c);
	}
	{   // 2. consumption: o1 produces p and consumes q, o2 produces q.
		//    goal p >= 2  =>  x1 = 2, and q drops to -2 so x2 >= 2  =>  h = 4.
		Case c; c.lp = mk(2);
		c.lp.rows.push_back({ { { 0, 1.0 } }, 2.0, "p" });
		c.lp.rows.push_back({ { { 0, -1.0 }, { 1, 1.0 } }, 0.0, "q" });
		c.feas = true; c.val = 4.0; cs.push_back(c);
	}
	{   // 3. capacity (th. 4): two routes, the cheaper one capped at 1.
		Case c; c.lp = mk(2);
		c.lp.cost = { 1.0, 5.0 };
		c.lp.upper = { 1.0, OperatorLP::kNoBound };
		c.lp.rows.push_back({ { { 0, 1.0 }, { 1, 1.0 } }, 3.0, "but" });
		c.feas = true; c.val = 1.0 + 2.0 * 5.0; cs.push_back(c);
	}
	{   // 4. infeasible (th. 3): the goal needs 3, the only route is capped at 2.
		Case c; c.lp = mk(1);
		c.lp.upper = { 2.0 };
		c.lp.rows.push_back({ { { 0, 1.0 } }, 3.0, "but" });
		c.feas = false; c.val = 0.0; cs.push_back(c);
	}
	{   // 5. constraint already satisfied (negative right-hand side): imposes nothing.
		Case c; c.lp = mk(1);
		c.lp.rows.push_back({ { { 0, 1.0 } }, 1.0, "but" });
		c.lp.rows.push_back({ { { 0, -1.0 } }, -4.0, "reserve" });
		c.feas = true; c.val = 1.0; cs.push_back(c);
	}
	// THE FUZZ: 60 random instances of the same shape, solved in EXACT rationals
	// by sympy.lpmin, 20 of them infeasible. Five
	// hand cases prove the theorems are covered; sixty random cases prove the
	// SOLVER, negative right-hand sides, active bounds and degeneracy included.
	// The expected value is ceil(exact optimum), which is what SolveOperatorLP
	// returns.
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
		// THE DUALITY CERTIFICATE. The exposed duals are believed only if they
		// CERTIFY the optimum on every case: dual feasibility (A'y - w <= c,
		// y >= 0, w >= 0) and strong duality (b'y - u'w = c'x, against sympy's
		// EXACT optimum, before rounding). A wrong dual here would designate
		// false quota hosts throughout the derivation.
		if(ok && c.feas) {
			double primal = 0.0;
			for(size_t j = 0; j < c.lp.n_ops; ++j)
				primal += c.lp.cost[j] * r.x[j];
			double dualv = 0.0;
			for(size_t i = 0; i < c.lp.rows.size(); ++i) {
				if(r.row_dual[i] < -1e-7)
					ok = false;   // y >= 0 violated
				dualv += r.row_dual[i] * c.lp.rows[i].rhs;
			}
			for(size_t j = 0; j < c.lp.n_ops; ++j) {
				if(r.bound_dual[j] < -1e-7)
					ok = false;   // w >= 0 violated
				if(c.lp.upper[j] < OperatorLP::kNoBound * 0.5)
					dualv -= r.bound_dual[j] * c.lp.upper[j];
				else if(r.bound_dual[j] > 1e-7)
					ok = false;   // bound dual with no finite bound
				double red_j = c.lp.cost[j] + r.bound_dual[j];
				for(size_t i = 0; i < c.lp.rows.size(); ++i)
					for(const auto& [jj, v] : c.lp.rows[i].coef)
						if(jj == j)
							red_j -= r.row_dual[i] * v;
				if(red_j < -1e-6)
					ok = false;   // dual feasibility violated
			}
			if(std::fabs(dualv - primal) > 1e-6)
				ok = false;       // strong duality violated
		}
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
	// PHYSICAL copies per code. It is that number, not mere presence, that decides
	// a "per COPY" capacity: three Kaleido Chick are worth three activations per
	// turn, one is worth only one.
	std::unordered_map<uint32_t, uint32_t> copies;
	for(uint32_t c : deck)
		++copies[db.Canonical(c)];

	// What has to be PRODUCED, and how many times. `fire[code]` is the number of
	// firings of `code`'s summon operator: it is the `x_o` of the balance sheet.
	std::unordered_map<uint32_t, uint32_t> fire, need_named;
	std::map<uint64_t, uint32_t> need_setcode;
	// THE GOAL IS CANONICALISED ON ENTRY. `--target 90590304` names Bagooska's
	// lying-down form; the table is built on the deck's codes, hence on
	// `90590303`. Without that reduction the card finds no recipe and vanishes
	// from the counts SILENTLY: one goal in two uncounted, without a word. It is
	// the same trap as everywhere else here: what does not match must be SEEN,
	// and here it is enough not to create the discrepancy.
	std::vector<std::pair<uint32_t, uint32_t>> work;
	for(const auto& [gc, gn] : goal)
		work.emplace_back(db.Canonical(gc), gn);
	std::unordered_map<uint32_t, uint32_t> seen_depth;
	for(size_t guard = 0; !work.empty() && guard < 4096; ++guard) {
		const auto [code, n] = work.back();
		work.pop_back();
		if(!n)
			continue;
		if(++seen_depth[code] > 8)   // cyclic graph: we bound, and we say so
			continue;
		need_named[code] += n;
		auto it = cards.find(code);
		if(it == cards.end() || it->second.recipes.empty())
			continue;   // base card: nothing to build
		// ONE ROUTE EXPANDED, and it is said in the header: we take the FIRST
		// non-empty declared recipe (a Pendulum card posts an empty Pendulum recipe
		// first), and we do not choose in the game's stead.
		const DeclaredRecipe* rp = nullptr;
		for(const DeclaredRecipe& cand : it->second.recipes)
			if(!cand.named.empty() || !cand.setcode.empty() ||
			   !cand.unresolved_counts.empty()) {
				rp = &cand;
				break;
			}
		if(!rp)
			continue;
		const DeclaredRecipe& r = *rp;
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

	// WHAT DOES NOT ENTER THE COUNTS MUST BE SEEN. A goal for which no recipe was
	// extracted used to vanish from the balance sheet WITHOUT A WORD, and a
	// balance sheet counting half a goal reads like a complete one. The cause is
	// stated, not guessed: either the card has no declared recipe, or its code
	// does not reduce to one of the deck's.
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

	// THE BALANCE ROW, PER PRODUCT, and it is HERE that the objection "the deck
	// RECYCLES" becomes a NUMBER instead of an argument.
	//
	// An extra deck monster can only be summoned as many times as copies of it
	// exist... PLUS whatever the recyclers put back into the zone. Counting the
	// copies alone would be STOCK reasoning, which is the exact mistake the
	// balance equation exists to avoid. So we count a FLOW: copies + recycling
	// capacity >= required firings.
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
				recyc_unbounded = true;   // SAFE direction: never prunes
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

	// THE ACQUISITIONS, AND THIS IS WHERE CAPACITY DECIDES. A NAMED material
	// absent from the deck can only come from an `EFFECT_ADD_CODE` granted state,
	// and the operator granting it has a DECLARED capacity. Three copies of the
	// goal need three acquisitions; a "per NAME" capacity allows ONE, whatever the
	// number of copies. That is one row of the balance sheet, and it is decided
	// without a single rollout.
	uint64_t add_code = 0;
	kt.Lookup("EFFECT_ADD_CODE", add_code);
	std::printf("\n  --- LES ACQUISITIONS (materiau NOMME absent du deck) ---\n");
	size_t n_acq = 0, n_bad = 0;
	for(const auto& [code, n] : need_named) {
		// Present in the deck (hand OR extra): it exists physically, nothing to
		// acquire. If it is present but NOT SUMMONABLE, the recursion will say so by
		// hitting ITS named material, which is absent.
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
