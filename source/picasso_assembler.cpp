#include "picasso.h"

//#define DEBUG
#define BUF g_outputBuf
#define NO_MORE_STACK (g_stackPos==MAX_STACK)

static const char* curFile = NULL;
static int curLine = -1;

std::vector<u32> g_outputBuf;

StackEntry g_stack[MAX_STACK];
int g_stackPos;

int g_opdescTable[MAX_OPDESC];
int g_opdescCount;
int g_opdescMasks[MAX_OPDESC];

Uniform g_uniformTable[MAX_UNIFORM];
int g_uniformCount;
static int fvecUnifPos = 0x20;
static int ivecUnifPos = 0x80;
static int boolUnifPos = 0x88;

Constant g_constantTable[MAX_CONSTANT];
int g_constantCount;
size_t g_constantSize;

u64 g_outputTable[MAX_OUTPUT];
int g_outputCount;

procTableType g_procTable;
labelTableType g_labels;
aliasTableType g_aliases;
relocTableType g_relocs;

static char* mystrtok_pos;
static char* mystrtok(char* str, const char* delim)
{
	if (!str) str = mystrtok_pos;
	if (!*str) return NULL;

	size_t pos = strcspn(str, delim);
	char* ret = str;
	str += pos;
	if (*str)
		*str++ = 0;
	mystrtok_pos = str;
	return ret;
}

static char* mystrtok_spc(char* str)
{
	char* ret = mystrtok(str, " \t");
	if (!ret) return NULL;
	if (*mystrtok_pos)
		for (; *mystrtok_pos && isspace(*mystrtok_pos); mystrtok_pos++);
	return ret;
}

static char* remove_comment(char* buf)
{
	char* pos = strchr(buf, ';');
	if (pos) *pos = 0;
	return buf;
}

static char* trim_whitespace(char* buf)
{
	if (!buf)
		return NULL;

	// Remove trailing whitespace
	int pos;
	for(pos = strlen(buf)-1; pos >= 0 && isspace(buf[pos]); pos --) buf[pos] = '\0';

	// Remove leading whitespace
	char* newbuf = buf;
	for(; isspace(*newbuf); newbuf ++);

	return newbuf;
}

static bool validateIdentifier(const char* id)
{
	int len = strlen(id);
	bool valid = true;
	for (int i = 0; valid && i < len; i ++)
	{
		int c = id[i];
		valid = isalpha(c) || c == '_' || c == '.' || (i > 0 && isdigit(c));
	}
	return valid;
}

static int throwError(const char* msg, ...)
{
	va_list v;

	fprintf(stderr, "%s:%d: error: ", curFile, curLine);

	va_start(v, msg);
	vfprintf(stderr, msg, v);
	va_end(v);

	return 1;
}

static int parseInt(char* pos, int& out, long long min, long long max)
{
	char* endptr = NULL;
	long long res = strtoll(pos, &endptr, 0);
	if (pos == endptr)
		return throwError("Invalid value: %s\n", pos);
	if (res < min || res > max)
		return throwError("Value out of range (%d..%u): %d\n", (int)min, (unsigned int)max, (int)res);
	out = res;
	return 0;
}

#define safe_call(x) do \
	{ \
		int _ = (x); \
		if (_ != 0) return _; \
	} while(0)

static int ProcessCommand(const char* cmd);
static int FixupRelocations();

int AssembleString(char* str, const char* initialFilename)
{
	curFile = initialFilename;
	curLine = 1;

	int nextLineIncr = 0;
	char* nextStr = NULL;
	for (; str; str = nextStr, curLine += nextLineIncr)
	{
		size_t len = strcspn(str, "\n");
		int linedelim = str[len];
		str[len] = 0;
		nextStr = linedelim ? (str + len + 1) : NULL;
		nextLineIncr = linedelim == '\n' ? 1 : 0;

		char* line = trim_whitespace(remove_comment(str));

		char* colonPos = NULL;
		for (;;)
		{
			colonPos = strchr(line, ':');
			if (!colonPos)
				break;
			*colonPos = 0;
			char* labelName = line;
			line = trim_whitespace(colonPos + 1);

			if (!validateIdentifier(labelName))
				return throwError("invalid label name: %s\n", labelName);

			std::pair<labelTableIter,bool> ret = g_labels.insert( std::pair<std::string,size_t>(labelName, BUF.size()) );
			if (!ret.second)
				return throwError("duplicate label: %s\n", labelName);

			//printf("Label: %s\n", labelName);
		};

		if (!*line)
			continue;

		if (*line == '#')
		{
			line = trim_whitespace(line + 1);
			nextLineIncr = 0;
			size_t pos = strcspn(line, " \t");
			line[pos] = 0;
			curLine = atoi(line);
			line = trim_whitespace(line + pos + 1);
			if (*line == '"')
			{
				line ++;
				line[strlen(line)-1] = 0;
			}
			curFile = line;
			continue;
		}

		char* tok = mystrtok_spc(line);
		safe_call(ProcessCommand(tok));
	}

	if (g_stackPos)
		return throwError("unclosed block(s)\n");

	safe_call(FixupRelocations());
	
	return 0;
}

int FixupRelocations()
{
	for (relocTableIter it = g_relocs.begin(); it != g_relocs.end(); ++it)
	{
		Relocation& r = *it;
		u32& inst = BUF[r.instPos];
		if (r.isProc)
		{
			procTableIter proc = g_procTable.find(r.target);
			if (proc == g_procTable.end())
				return throwError("procedure '%s' is undefined\n", r.target);
			u32 dst = proc->second.first;
			u32 num = proc->second.second;
			inst &= ~0x3FFFFF;
			inst |= num | (dst << 10);
		} else
		{
			labelTableIter lbl = g_labels.find(r.target);
			if (lbl == g_labels.end())
				return throwError("label '%s' is undefined\n", r.target);
			u32 dst = lbl->second;
			inst &= ~(0xFFF << 10);
			inst |= dst << 10;
		}
	}
	return 0;
}

// --------------------------------------------------------------------
// Commands
// --------------------------------------------------------------------

static char* nextArg()
{
	return trim_whitespace(mystrtok(NULL, ","));
}

static char* nextArgCParen()
{
	return trim_whitespace(mystrtok(NULL, "("));
}

static char* nextArgSpc()
{
	return trim_whitespace(mystrtok_spc(NULL));
}

static int missingParam()
{
	return throwError("missing parameter\n");
}

typedef struct
{
	const char* name;
	int (* func) (const char*, int);
	int opcode;
} cmdTableType;

#define NEXT_ARG(_varName) char* _varName; do \
	{ \
		_varName = nextArg(); \
		if (!_varName) return missingParam(); \
	} while (0)

#define NEXT_ARG_SPC(_varName) char* _varName; do \
	{ \
		_varName = nextArgSpc(); \
		if (!_varName) return missingParam(); \
	} while (0)

#define NEXT_ARG_CPAREN(_varName) char* _varName; do \
	{ \
		_varName = nextArgCParen(); \
		if (!_varName) return missingParam(); \
	} while (0)

#define NEXT_ARG_OPT(_varName, _opt) char* _varName; do \
	{ \
		_varName = nextArg(); \
		if (!_varName) _varName = (char*)(_opt); \
	} while (0)

#define DEF_COMMAND(name) \
	static int cmd_##name(const char* cmdName, int opcode)

#define DEC_COMMAND(name, fun) \
	{ #name, cmd_##fun, MAESTRO_##name }

#define DEF_DIRECTIVE(name) \
	static int dir_##name(const char* cmdName, int dirParam)

#define DEC_DIRECTIVE(name) \
	{ #name, dir_##name, 0 }

#define DEC_DIRECTIVE2(name, fun, opc) \
	{ #name, dir_##fun, opc }

static int ensureNoMoreArgs()
{
	return nextArg() ? throwError("too many parameters\n") : 0;
}

static int duplicateIdentifier(const char* id)
{
	return throwError("identifier already used: %s\n", id);
}

static int ensureTarget(const char* target)
{
	if (!validateIdentifier(target))
		return throwError("invalid target: %s\n", target);
	return 0;
}

static inline int ensure_valid_dest(int reg, const char* name)
{
	if (reg < 0x00 || reg >= 0x20)
		return throwError("invalid destination register: %s\n", name);
	return 0;
}

static inline int ensure_valid_src1(int reg, const char* name)
{
	if (reg < 0x00 || reg >= 0x80)
		return throwError("invalid source1 register: %s\n", name);
	return 0;
}

static inline int ensure_valid_src2(int reg, const char* name)
{
	if (reg < 0x00 || reg >= 0x20)
		return throwError("invalid source2 register: %s\n", name);
	return 0;
}

static inline int ensure_valid_ireg(int reg, const char* name)
{
	if (reg < 0x80 || reg >= 0x88)
		return throwError("invalid integer vector uniform: %s\n", name);
	return 0;
}

static inline int ensure_valid_breg(int reg, const char* name)
{
	if (reg < 0x88 || reg >= 0x98)
		return throwError("invalid boolean uniform: %s\n", name);
	return 0;
}

static inline int ensure_valid_condop(int condop, const char* name)
{
	if (condop < 0)
		return throwError("invalid conditional operator: %s\n", name);
	return 0;
}

#define ENSURE_NO_MORE_ARGS() safe_call(ensureNoMoreArgs())

#define ARG_TO_INT(_varName, _argName, _min, _max) \
	int _varName = 0; \
	safe_call(parseInt(_argName, _varName, _min, _max))

#define ARG_TO_REG(_varName, _argName) \
	int _varName = 0, _varName##Sw = 0; \
	safe_call(parseReg(_argName, _varName, _varName##Sw));

#define ARG_TO_REG2(_varName, _argName) \
	int _varName = 0, _varName##Sw = 0, _varName##Idx = 0; \
	safe_call(parseReg(_argName, _varName, _varName##Sw, &_varName##Idx));

#define ARG_TO_CONDOP(_varName, _argName) \
	int _varName = parseCondOp(_argName); \
	safe_call(ensure_valid_condop(_varName, _argName))

#define ARG_TARGET(_argName) \
	safe_call(ensureTarget(_argName))

#define ARG_TO_DEST_REG(_reg, _name) \
	ARG_TO_REG(_reg, _name); \
	safe_call(ensure_valid_dest(_reg, _name))

#define ARG_TO_SRC1_REG(_reg, _name) \
	ARG_TO_REG(_reg, _name); \
	safe_call(ensure_valid_src1(_reg, _name))

#define ARG_TO_SRC1_REG2(_reg, _name) \
	ARG_TO_REG2(_reg, _name); \
	safe_call(ensure_valid_src1(_reg, _name))

#define ARG_TO_SRC2_REG(_reg, _name) \
	ARG_TO_REG(_reg, _name); \
	safe_call(ensure_valid_src2(_reg, _name))

#define ARG_TO_IREG(_reg, _name) \
	ARG_TO_REG(_reg, _name); \
	safe_call(ensure_valid_ireg(_reg, _name))

#define ARG_TO_BREG(_reg, _name) \
	ARG_TO_REG(_reg, _name); \
	safe_call(ensure_valid_breg(_reg, _name))

static int parseSwizzling(const char* b)
{
	int i, out = 0, q = COMP_X;
	for (i = 0; b[i] && i < 4; i ++)
	{
		switch (tolower(b[i]))
		{
			case 'x': q = COMP_X; break;
			case 'y': q = COMP_Y; break;
			case 'z': q = COMP_Z; break;
			case 'w': q = COMP_W; break;
			default: return -1;
		}
		out |= SWIZZLE_COMP(i, q);
	}
	if (b[i])
		return -1;
	// Fill in missing bits
	for (int j = i; j < 4; j ++)
		out |= SWIZZLE_COMP(j, q);
	return out<<1;
}

static int maskFromSwizzling(int sw)
{
	sw >>= 1; // get rid of negation bit
	int out = 0;
	for (int i = 0; i < 4; i ++)
		out |= BIT(3-((sw>>(i*2))&3));
	return out;
}

static int findOrAddOpdesc(int& out, int opdesc, int mask)
{
	for (int i = 0; i < g_opdescCount; i ++)
	{
		int minMask = mask & g_opdescMasks[i];
		if ((opdesc&minMask) == (g_opdescTable[i]&minMask))
		{
			// Save extra bits, if any
			g_opdescTable[i] |= opdesc & (mask^minMask);
			g_opdescMasks[i] |= mask;
			out = i;
			return 0;
		}
	}
	if (g_opdescCount == MAX_OPDESC)
		return throwError("too many operand descriptors (limit is %d)\n", MAX_OPDESC);
	g_opdescTable[g_opdescCount] = opdesc;
	g_opdescMasks[g_opdescCount] = mask;
	out = g_opdescCount++;
	return 0;
}

static inline bool isregp(int x)
{
	x = tolower(x);
	return x=='o' || x=='v' || x=='r' || x=='c' || x=='i' || x=='b';
}

static inline int convertIdxRegName(const char* reg)
{
	if (stricmp(reg, "a0")==0) return 1;
	if (stricmp(reg, "a1")==0) return 2;
	if (stricmp(reg, "a2")==0 || stricmp(reg, "lcnt")==0) return 2;
	return 0;
}

static inline int parseCondOp(const char* name)
{
	if (stricmp(name, "eq")==0) return COND_EQ;
	if (stricmp(name, "ne")==0) return COND_NE;
	if (stricmp(name, "lt")==0) return COND_LT;
	if (stricmp(name, "le")==0) return COND_LE;
	if (stricmp(name, "gt")==0) return COND_GT;
	if (stricmp(name, "ge")==0) return COND_GE;
	if (*name == '6') return COND_UNK1;
	if (*name == '7') return COND_UNK2;
	return -1;
}

static int parseReg(char* pos, int& outReg, int& outSw, int* idxType = NULL)
{
	outReg = 0;
	outSw = DEFAULT_OPSRC;
	if (idxType) *idxType = 0;
	if (*pos == '-')
	{
		pos++;
		outSw |= 1; // negation bit
	}
	char* dotPos = strchr(pos, '.');
	if (dotPos)
	{
		*dotPos++ = 0;
		outSw = parseSwizzling(dotPos) | (outSw&1);
		if (outSw < 0)
			return throwError("invalid swizzling mask: %s\n", dotPos);
	}
	int regOffset = 0;
	char* offPos = strchr(pos, '[');
	if (offPos)
	{
		char* closePos = strchr(offPos, ']');
		if (!closePos)
			return throwError("missing closing bracket: %s\n", pos);
		*closePos = 0;
		*offPos++ = 0;
		offPos = trim_whitespace(offPos);

		// Check for idxreg+offset
		int temp = convertIdxRegName(offPos);
		if (temp>0)
		{
			if (!idxType)
				return throwError("index register not allowed here: %s\n", offPos);
			*idxType = temp;
		} else do
		{
			char* plusPos = strchr(offPos, '+');
			if (!plusPos)
				break;
			if (!idxType)
				return throwError("index register not allowed here: %s\n", offPos);
			*plusPos++ = 0;
			char* idxRegName = trim_whitespace(offPos);
			offPos = trim_whitespace(plusPos);
			*idxType = convertIdxRegName(idxRegName);
			if (*idxType < 0)
				return throwError("invalid index register: %s\n", idxRegName);
		} while (0);

		regOffset = atoi(offPos);
		if (regOffset < 0)
			return throwError("invalid register offset: %s\n", offPos);
	}
	aliasTableIter it = g_aliases.find(pos);
	if (it != g_aliases.end())
	{
		int x = it->second;
		outReg = x & 0xFF;
		outReg += regOffset;
		outSw ^= (x>>8)&1;
		x >>= 9;
		// Combine swizzling
		int temp = outSw & 1;
		for (int j = 0; j < 4; j ++)
		{
			int comp = (outSw >> (7 - j*2)) & 3;
			comp = (x >> (6 - comp*2)) & 3;
			temp |= SWIZZLE_COMP(j, comp)<<1;
		}
		outSw = temp;
		return 0;
	}

	if (!isregp(pos[0]) || !isdigit(pos[1]))
		return throwError("invalid register: %s\n", pos);

	safe_call(parseInt(pos+1, outReg, 0, 255));
	switch (*pos)
	{
		case 'o': // Output registers
		case 'v': // Input attributes
			if (outReg < 0x00 || outReg >= 0x08)
				return throwError("invalid input/output register: %s(%d)\n", pos);
			break;
		case 'r': // Temporary registers
			outReg += 0x10;
			if (outReg < 0x10 || outReg >= 0x20)
				return throwError("invalid temporary register: %s(%d)\n", pos);
			break;
		case 'c': // Floating-point vector uniform registers
			outReg += 0x20;
			if (outReg < 0x20 || outReg >= 0x80)
				return throwError("invalid floating-point vector uniform register: %s(%d)\n", pos);
			break;
		case 'i': // Integer vector uniforms
			outReg += 0x80;
			if (outReg < 0x80 || outReg >= 0x88)
				return throwError("invalid integer vector uniform register: %s(%d)\n", pos);
			break;
		case 'b': // Boolean uniforms
			outReg += 0x88;
			if (outReg < 0x88 || outReg >= 0x98)
				return throwError("invalid boolean uniform register: %s(%d)\n", pos);
			break;
	}
	outReg += regOffset;
	return 0;
}

static int parseCondExpOp(char* str, u32& outFlags, int& which)
{
	int negation = 0;
	for (; *str == '!'; str++) negation ^= 1;
	if (stricmp(str, "cmp.x")==0)
	{
		which = 1;
		outFlags ^= negation<<25;
		return 0;
	}
	if (stricmp(str, "cmp.y")==0)
	{
		which = 0;
		outFlags ^= negation<<24;
		return 0;
	}
	return throwError("invalid condition register: %s\n", str);
}

static int parseCondExp(char* str, u32& outFlags)
{
	outFlags = BIT(24) | BIT(25);
	size_t len = strlen(str);
	size_t pos = strcspn(str, "&|");
	int op2 = -1;
	if (pos < len)
	{
		char* str2 = str + pos;
		int type = *str2;
		*str2++ = 0;
		if (*str2 == type)
			str2++;
		str = trim_whitespace(str);
		str2 = trim_whitespace(str2);
		if (type == '&')
			outFlags |= 1<<22;
		safe_call(parseCondExpOp(str2, outFlags, op2));
	}
	int op1 = -1;
	safe_call(parseCondExpOp(str, outFlags, op1));
	if (op1 == op2)
		return throwError("condition register checked twice\n");
	if (op2 < 0)
		outFlags |= (op1+2)<<22;
	return 0;
}

DEF_COMMAND(format0)
{
	ENSURE_NO_MORE_ARGS();

	BUF.push_back(FMT_OPCODE(opcode));
	return 0;
}

DEF_COMMAND(format1)
{
	NEXT_ARG(destName);
	NEXT_ARG(src1Name);
	NEXT_ARG(src2Name);
	ENSURE_NO_MORE_ARGS();

	ARG_TO_DEST_REG(rDest, destName);
	ARG_TO_SRC1_REG2(rSrc1, src1Name);
	ARG_TO_SRC2_REG(rSrc2, src2Name);

	int opdesc = 0;
	safe_call(findOrAddOpdesc(opdesc, OPDESC_MAKE(maskFromSwizzling(rDestSw), rSrc1Sw, rSrc2Sw, 0), OPDESC_MASK_D12));

#ifdef DEBUG
	printf("%s:%02X d%02X, d%02X, d%02X (0x%X)\n", cmdName, opcode, rDest, rSrc1, rSrc2, opdesc);
#endif
	BUF.push_back(FMT_OPCODE(opcode) | opdesc | (rSrc2<<7) | (rSrc1<<12) | (rSrc1Idx<<19) | (rDest<<21));

	return 0;
}

DEF_COMMAND(format1i)
{
	NEXT_ARG(destName);
	NEXT_ARG(src1Name);
	NEXT_ARG(src2Name);
	ENSURE_NO_MORE_ARGS();

	ARG_TO_DEST_REG(rDest, destName);
	ARG_TO_SRC2_REG(rSrc1, src1Name);
	ARG_TO_SRC1_REG2(rSrc2, src2Name);

	int opdesc = 0;
	safe_call(findOrAddOpdesc(opdesc, OPDESC_MAKE(maskFromSwizzling(rDestSw), rSrc1Sw, rSrc2Sw, 0), OPDESC_MASK_D12));

#ifdef DEBUG
	printf("%s:%02X d%02X, d%02X, d%02X (0x%X)\n", cmdName, opcode, rDest, rSrc1, rSrc2, opdesc);
#endif
	BUF.push_back(FMT_OPCODE(opcode) | opdesc | (rSrc2<<7) | (rSrc1<<14) | (rSrc2Idx<<19) | (rDest<<21));

	return 0;
}

DEF_COMMAND(format1u)
{
	NEXT_ARG(destName);
	NEXT_ARG(src1Name);
	ENSURE_NO_MORE_ARGS();

	ARG_TO_DEST_REG(rDest, destName);
	ARG_TO_SRC1_REG2(rSrc1, src1Name);

	int opdesc = 0;
	safe_call(findOrAddOpdesc(opdesc, OPDESC_MAKE(maskFromSwizzling(rDestSw), rSrc1Sw, 0, 0), OPDESC_MASK_D1));

#ifdef DEBUG
	printf("%s:%02X d%02X, d%02X (0x%X)\n", cmdName, opcode, rDest, rSrc1, opdesc);
#endif
	BUF.push_back(FMT_OPCODE(opcode) | opdesc | (rSrc1<<12) | (rSrc1Idx<<19) | (rDest<<21));

	return 0;
}

DEF_COMMAND(format1c)
{
	NEXT_ARG(src1Name);
	NEXT_ARG(cmpxName);
	NEXT_ARG(cmpyName);
	NEXT_ARG(src2Name);
	ENSURE_NO_MORE_ARGS();

	ARG_TO_SRC1_REG2(rSrc1, src1Name);
	ARG_TO_CONDOP(cmpx, cmpxName);
	ARG_TO_CONDOP(cmpy, cmpyName);
	ARG_TO_SRC2_REG(rSrc2, src2Name);

	int opdesc = 0;
	safe_call(findOrAddOpdesc(opdesc, OPDESC_MAKE(0, rSrc1Sw, rSrc2Sw, 0), OPDESC_MASK_12));

#ifdef DEBUG
	printf("%s:%02X d%02X, %d, %d, d%02X (0x%X)\n", cmdName, opcode, rSrc1, cmpx, cmpy, rSrc2, opdesc);
#endif
	BUF.push_back(FMT_OPCODE(opcode) | opdesc | (rSrc2<<7) | (rSrc1<<12) | (rSrc1Idx<<19) | (cmpy<<21) | (cmpx<<24));

	return 0;
}

DEF_COMMAND(format5)
{
	NEXT_ARG(destName);
	NEXT_ARG(src1Name);
	NEXT_ARG(src2Name);
	NEXT_ARG(src3Name);
	ENSURE_NO_MORE_ARGS();

	ARG_TO_DEST_REG(rDest, destName);
	ARG_TO_SRC1_REG(rSrc1, src1Name);
	ARG_TO_SRC1_REG(rSrc2, src2Name);
	ARG_TO_SRC2_REG(rSrc3, src3Name);

	int opdesc = 0;
	safe_call(findOrAddOpdesc(opdesc, OPDESC_MAKE(maskFromSwizzling(rDestSw), rSrc1Sw, rSrc2Sw, rSrc3Sw), OPDESC_MASK_D123));

	if (opdesc >= 32)
		return throwError("opdesc allocation error\n");

#ifdef DEBUG
	printf("%s:%02X d%02X, d%02X, d%02X, d%02X (0x%X)\n", cmdName, opcode, rDest, rSrc1, rSrc2, rSrc3, opdesc);
#endif
	BUF.push_back(FMT_OPCODE(opcode) | opdesc | (rSrc3<<5) | (rSrc2<<10) | (rSrc1<<17) | (rDest<<24));

	return 0;
}

DEF_COMMAND(format5i)
{
	NEXT_ARG(destName);
	NEXT_ARG(src1Name);
	NEXT_ARG(src2Name);
	NEXT_ARG(src3Name);
	ENSURE_NO_MORE_ARGS();

	ARG_TO_DEST_REG(rDest, destName);
	ARG_TO_SRC1_REG(rSrc1, src1Name);
	ARG_TO_SRC2_REG(rSrc2, src2Name);
	ARG_TO_SRC1_REG(rSrc3, src3Name);

	int opdesc = 0;
	safe_call(findOrAddOpdesc(opdesc, OPDESC_MAKE(maskFromSwizzling(rDestSw), rSrc1Sw, rSrc2Sw, rSrc3Sw), OPDESC_MASK_D123));

	if (opdesc >= 32)
		return throwError("opdesc allocation error\n");

#ifdef DEBUG
	printf("%s:%02X d%02X, d%02X, d%02X, d%02X (0x%X)\n", cmdName, opcode, rDest, rSrc1, rSrc2, rSrc3, opdesc);
#endif
	BUF.push_back(FMT_OPCODE(opcode) | opdesc | (rSrc3<<5) | (rSrc2<<12) | (rSrc1<<17) | (rDest<<24));

	return 0;
}

DEF_COMMAND(formatmova)
{
	NEXT_ARG(src1Name);
	ENSURE_NO_MORE_ARGS();

	ARG_TO_SRC1_REG2(rSrc1, src1Name);

	int opdesc = 0;
	safe_call(findOrAddOpdesc(opdesc, OPDESC_MAKE(0, rSrc1Sw, 0, 0), OPDESC_MASK_1));

#ifdef DEBUG
	printf("%s:%02X d%02X (0x%X)\n", cmdName, opcode, rSrc1, opdesc);
#endif
	BUF.push_back(FMT_OPCODE(opcode) | opdesc | (rSrc1<<12) | (rSrc1Idx<<19));

	return 0;
}

DEF_COMMAND(formatcall)
{
	NEXT_ARG(procName);
	ENSURE_NO_MORE_ARGS();

	ARG_TARGET(procName);

	Relocation r;
	r.instPos = BUF.size();
	r.target = procName;
	r.isProc = true;
	g_relocs.push_back(r);

	BUF.push_back(FMT_OPCODE(opcode));

#ifdef DEBUG
	printf("%s:%02X %s\n", cmdName, opcode, procName);
#endif
	return 0;
}

DEF_COMMAND(formatfor)
{
	NEXT_ARG(regName);
	ENSURE_NO_MORE_ARGS();

	ARG_TO_IREG(regId, regName);

	if (NO_MORE_STACK)
		return throwError("too many nested blocks\n");

	StackEntry& elem = g_stack[g_stackPos++];
	elem.type = SE_FOR;
	elem.pos = BUF.size();

	BUF.push_back(FMT_OPCODE(opcode) | ((regId-0x80) << 22));

#ifdef DEBUG
	printf("%s:%02X d%02X\n", cmdName, opcode, regId);
#endif
	return 0;
}

DEF_COMMAND(format2)
{
	NEXT_ARG(condExp);

	u32 instruction = 0;
	safe_call(parseCondExp(condExp, instruction));

	switch (opcode)
	{
		case MAESTRO_BREAKC:
		{
			ENSURE_NO_MORE_ARGS();

#ifdef DEBUG
			printf("%s:%02X %s\n", cmdName, opcode, condExp);
#endif
			break;
		}

		case MAESTRO_CALLC:
		case MAESTRO_JMPC:
		{
			NEXT_ARG(targetName);
			ENSURE_NO_MORE_ARGS();

			ARG_TARGET(targetName);

			Relocation r;
			r.instPos = BUF.size();
			r.target = targetName;
			r.isProc = opcode==MAESTRO_CALLC;
			g_relocs.push_back(r);

#ifdef DEBUG
			printf("%s:%02X %s, %s\n", cmdName, opcode, condExp, targetName);
#endif
			break;
		}

		case MAESTRO_IFC:
		{
			ENSURE_NO_MORE_ARGS();

			if (NO_MORE_STACK)
				return throwError("too many nested blocks\n");

			StackEntry& elem = g_stack[g_stackPos++];
			elem.type = SE_IF;
			elem.pos = BUF.size();
			elem.uExtra = 0;

#ifdef DEBUG
			printf("%s:%02X %s\n", cmdName, opcode, condExp);
#endif
			break;
		}
	}

	BUF.push_back(FMT_OPCODE(opcode) | instruction);

	return 0;
}

DEF_COMMAND(format3)
{
	NEXT_ARG(regName);

	ARG_TO_BREG(regId, regName);

	switch (opcode)
	{
		case MAESTRO_CALLU:
		case MAESTRO_JMPU:
		{
			NEXT_ARG(targetName);
			ENSURE_NO_MORE_ARGS();

			ARG_TARGET(targetName);

			Relocation r;
			r.instPos = BUF.size();
			r.target = targetName;
			r.isProc = opcode==MAESTRO_CALLU;
			g_relocs.push_back(r);

#ifdef DEBUG
			printf("%s:%02X d%02X, %s\n", cmdName, opcode, regId, targetName);
#endif
			break;
		}

		case MAESTRO_IFU:
		{
			ENSURE_NO_MORE_ARGS();

			if (NO_MORE_STACK)
				return throwError("too many nested blocks\n");

			StackEntry& elem = g_stack[g_stackPos++];
			elem.type = SE_IF;
			elem.pos = BUF.size();
			elem.uExtra = 0;

#ifdef DEBUG
			printf("%s:%02X d%02X\n", cmdName, opcode, regId);
#endif
			break;
		}
	}

	BUF.push_back(FMT_OPCODE(opcode) | ((regId-0x88) << 22));

	return 0;
}

static const cmdTableType cmdTable[] =
{
	DEC_COMMAND(NOP, format0),
	DEC_COMMAND(END, format0),

	DEC_COMMAND(ADD, format1),
	DEC_COMMAND(DP3, format1),
	DEC_COMMAND(DP4, format1),
	DEC_COMMAND(DPH, format1),
	DEC_COMMAND(MUL, format1),
	DEC_COMMAND(SGE, format1),
	DEC_COMMAND(SLT, format1),
	DEC_COMMAND(MAX, format1),
	DEC_COMMAND(MIN, format1),

	DEC_COMMAND(DPHI, format1i),
	DEC_COMMAND(SGEI, format1i),
	DEC_COMMAND(SLTI, format1i),

	DEC_COMMAND(EX2, format1u),
	DEC_COMMAND(LG2, format1u),
	DEC_COMMAND(FLR, format1u),
	DEC_COMMAND(RCP, format1u),
	DEC_COMMAND(RSQ, format1u),
	DEC_COMMAND(MOV, format1u),

	DEC_COMMAND(MOVA, formatmova),

	DEC_COMMAND(CMP, format1c),

	DEC_COMMAND(CALL, formatcall),

	DEC_COMMAND(FOR, formatfor),

	DEC_COMMAND(BREAKC, format2),
	DEC_COMMAND(CALLC, format2),
	DEC_COMMAND(IFC, format2),
	DEC_COMMAND(JMPC, format2),

	DEC_COMMAND(CALLU, format3),
	DEC_COMMAND(IFU, format3),
	DEC_COMMAND(JMPU, format3),

	DEC_COMMAND(MADI, format5i),
	DEC_COMMAND(MAD, format5),

	{ NULL, NULL },
};

// --------------------------------------------------------------------
// Directives
// --------------------------------------------------------------------

DEF_DIRECTIVE(proc)
{
	NEXT_ARG(procName);
	ENSURE_NO_MORE_ARGS();

	if (NO_MORE_STACK)
		return throwError("too many nested blocks\n");

	StackEntry& elem = g_stack[g_stackPos++];
	elem.type = SE_PROC;
	elem.pos = BUF.size();
	elem.strExtra = procName;

	if (g_procTable.find(procName) != g_procTable.end())
		return throwError("proc already exists: %s\n", procName);

#ifdef DEBUG
	printf("Defining %s\n", procName);
#endif
	return 0;
}

DEF_DIRECTIVE(else)
{
	ENSURE_NO_MORE_ARGS();
	if (!g_stackPos)
		return throwError(".else with unmatched IF\n");

	StackEntry& elem = g_stack[g_stackPos-1];
	if (elem.type != SE_IF)
		return throwError(".else with unmatched IF\n");
	if (elem.uExtra)
		return throwError("spurious .else\n");

	u32 curPos = BUF.size();
	elem.uExtra = curPos;
	u32& inst = BUF[elem.pos];
	inst &= ~(0xFFF << 10);
	inst |= curPos << 10;

#ifdef DEBUG
	printf("ELSE\n");
#endif

	return 0;
}

DEF_DIRECTIVE(end)
{
	ENSURE_NO_MORE_ARGS();
	if (!g_stackPos)
		return throwError(".end with unmatched block\n");
	
	StackEntry& elem = g_stack[--g_stackPos];
	u32 curPos = BUF.size();
	u32 size = curPos - elem.pos;

	switch (elem.type)
	{
		case SE_PROC:
		{
#ifdef DEBUG
			printf("proc: %s(%u, size:%u)\n", elem.strExtra, elem.pos, size);
#endif
			g_procTable.insert( std::pair<std::string, procedure>(elem.strExtra, procedure(elem.pos, size)) );
			break;
		}

		case SE_FOR:
		{
#ifdef DEBUG
			printf("ENDFOR\n");
#endif
			u32& inst = BUF[elem.pos];
			inst &= ~(0xFFF << 10);
			inst |= (curPos-1) << 10;
			break;
		}

		case SE_IF:
		{
#ifdef DEBUG
			printf("ENDIF\n");
#endif
			u32& inst = BUF[elem.pos];
			if (!elem.uExtra)
			{
				// IF with no ELSE
				inst &= ~(0xFFF << 10);
				inst |= curPos << 10;
			} else
			{
				// IF with an ELSE
				inst &= ~0x3FF;
				inst |= curPos - elem.uExtra;
			}
			break;
		}
	}

	return 0;
}

DEF_DIRECTIVE(alias)
{
	NEXT_ARG_SPC(aliasName);
	NEXT_ARG_SPC(aliasReg);
	ENSURE_NO_MORE_ARGS();

	if (!validateIdentifier(aliasName))
		return throwError("invalid alias name: %s\n", aliasName);
	if (isregp(aliasName[0]) && isdigit(aliasName[1]))
		return throwError("cannot redefine register\n");
	ARG_TO_REG(rAlias, aliasReg);

	if (g_aliases.find(aliasName) != g_aliases.end())
		return duplicateIdentifier(aliasName);

	g_aliases.insert( std::pair<std::string,int>(aliasName, rAlias | (rAliasSw<<8)) );
	return 0;
}

static inline int& getAllocVar(int type, int& bound)
{
	switch (type)
	{
		default:
		case UTYPE_FVEC: bound = 0x80; return fvecUnifPos;
		case UTYPE_IVEC: bound = 0x88; return ivecUnifPos;
		case UTYPE_BOOL: bound = 0x98; return boolUnifPos;
	}
}

DEF_DIRECTIVE(uniform)
{
	int bound;
	int& uniformPos = getAllocVar(dirParam, bound);

	for (;;)
	{
		char* argText = nextArg();
		if (!argText) break;

		int uSize = 1;
		char* sizePos = strchr(argText, '[');
		if (sizePos)
		{
			char* closePos = strchr(sizePos, ']');
			if (!closePos)
				return throwError("missing closing bracket: %s\n", argText);
			*closePos = 0;
			*sizePos++ = 0;
			sizePos = trim_whitespace(sizePos);
			uSize = atoi(sizePos);
			if (uSize < 1)
				return throwError("invalid uniform size: %s[%s]\n", argText, sizePos);
		}
		if (!validateIdentifier(argText))
			return throwError("invalid uniform name: %s\n", argText);
		if ((uniformPos+uSize) >= bound)
			return throwError("not enough uniform registers: %s[%d]\n", argText, uSize);
		if (g_uniformCount == MAX_UNIFORM)
			return throwError("too many uniforms: %s[%d]\n", argText, uSize);
		if (g_aliases.find(argText) != g_aliases.end())
			return duplicateIdentifier(argText);

		Uniform& uniform = g_uniformTable[g_uniformCount++];
		uniform.name = argText;
		uniform.pos = uniformPos;
		uniform.size = uSize;
		uniform.type = dirParam;
		uniformPos += uSize;
		g_aliases.insert( std::pair<std::string,int>(argText, uniform.pos | (DEFAULT_OPSRC<<8)) );

#ifdef DEBUG
		printf("uniform %s[%d] @ d%02X:d%02X\n", argText, uSize, uniform.pos, uniform.pos+uSize-1);
#endif
	}
	return 0;
}

DEF_DIRECTIVE(const)
{
	int bound;
	int& uniformPos = getAllocVar(dirParam, bound);

	NEXT_ARG_CPAREN(constName);
	NEXT_ARG(arg0Text);
	NEXT_ARG(arg1Text);
	NEXT_ARG(arg2Text);
	char* arg3Text = mystrtok_pos;
	if (!mystrtok_pos) return missingParam();
	char* parenPos = strchr(arg3Text, ')');
	if (!parenPos) return throwError("invalid syntax\n");
	*parenPos = 0;
	arg3Text = trim_whitespace(arg3Text);

	if (g_constantCount == MAX_CONSTANT || uniformPos>=bound)
		return throwError("not enough space for constant\n");

	if (g_aliases.find(constName) != g_aliases.end())
		return duplicateIdentifier(constName);

	Constant& ct = g_constantTable[g_constantCount++];
	ct.regId = uniformPos++;
	ct.type = dirParam;
	if (dirParam == UTYPE_FVEC)
	{
		ct.fparam[0] = atof(arg0Text);
		ct.fparam[1] = atof(arg1Text);
		ct.fparam[2] = atof(arg2Text);
		ct.fparam[3] = atof(arg3Text);
		g_constantSize += 4 + 4*4;
	} else if (dirParam == UTYPE_IVEC)
	{
		ct.iparam[0] = atoi(arg0Text) & 0xFF;
		ct.iparam[1] = atoi(arg1Text) & 0xFF;
		ct.iparam[2] = atoi(arg2Text) & 0xFF;
		ct.iparam[3] = atoi(arg3Text) & 0xFF;
		g_constantSize += 4 + 4;
	}

	g_aliases.insert( std::pair<std::string,int>(constName, ct.regId | (DEFAULT_OPSRC<<8)) );

#ifdef DEBUG
	if (dirParam == UTYPE_FVEC)
		printf("constant %s(%f, %f, %f, %f) @ d%02X\n", constName, ct.fparam[0], ct.fparam[1], ct.fparam[2], ct.fparam[3], ct.regId);
	else if (dirParam == UTYPE_IVEC)
		printf("constant %s(%u, %u, %u, %u) @ d%02X\n", constName, ct.iparam[0], ct.iparam[1], ct.iparam[2], ct.iparam[3], ct.regId);
#endif
	return 0;
};

static int parseOutType(const char* text)
{
	if (stricmp(text,"pos")==0 || stricmp(text,"position")==0)
		return OUTTYPE_POS;
	if (stricmp(text,"clr")==0 || stricmp(text,"color")==0)
		return OUTTYPE_CLR;
	if (stricmp(text,"tcoord0")==0 || stricmp(text,"texcoord0")==0)
		return OUTTYPE_TCOORD0;
	if (stricmp(text,"tcoord1")==0 || stricmp(text,"texcoord1")==0)
		return OUTTYPE_TCOORD1;
	if (stricmp(text,"tcoord2")==0 || stricmp(text,"texcoord2")==0)
		return OUTTYPE_TCOORD2;
	return -1;
}

DEF_DIRECTIVE(out)
{
	NEXT_ARG_SPC(outName);
	NEXT_ARG_SPC(outType);
	ENSURE_NO_MORE_ARGS();

	if (!validateIdentifier(outName))
		return throwError("invalid identifier: %s\n", outName);

	int sw = DEFAULT_OPSRC;
	char* dotPos = strchr(outType, '.');
	if (dotPos)
	{
		*dotPos++ = 0;
		sw = parseSwizzling(dotPos);
		if (sw < 0)
			return throwError("invalid output mask: %s\n", dotPos);
	}
	int mask = maskFromSwizzling(sw);
	int type = parseOutType(outType);
	if (type < 0)
		return throwError("invalid output type: %s\n", outType);

	if (g_outputCount==MAX_OUTPUT)
		return throwError("too many outputs\n");

	if (g_aliases.find(outName) != g_aliases.end())
		return duplicateIdentifier(outName);

	int oid = g_outputCount;

#ifdef DEBUG
	printf("output %s <- o%d (%d:%X)\n", outName, oid, type, mask);
#endif

	g_outputTable[g_outputCount++] = OUTPUT_MAKE(type, oid, mask);
	g_aliases.insert( std::pair<std::string,int>(outName, oid | (sw<<8)) );
	return 0;
}

static const cmdTableType dirTable[] =
{
	DEC_DIRECTIVE(proc),
	DEC_DIRECTIVE(else),
	DEC_DIRECTIVE(end),
	DEC_DIRECTIVE(alias),
	DEC_DIRECTIVE2(fvec, uniform, UTYPE_FVEC),
	DEC_DIRECTIVE2(ivec, uniform, UTYPE_IVEC),
	DEC_DIRECTIVE2(bool, uniform, UTYPE_BOOL),
	DEC_DIRECTIVE2(constf, const, UTYPE_FVEC),
	DEC_DIRECTIVE2(consti, const, UTYPE_IVEC),
	DEC_DIRECTIVE(out),
	{ NULL, NULL },
};

int ProcessCommand(const char* cmd)
{
	const cmdTableType* table = cmdTable;
	if (*cmd == '.')
	{
		cmd ++;
		table = dirTable;
	} else if (!g_stackPos)
		return throwError("instruction outside block\n");

	for (int i = 0; table[i].name; i ++)
		if (stricmp(table[i].name, cmd) == 0)
			return table[i].func(cmd, table[i].opcode);

	return throwError("invalid instruction: %s\n", cmd);
}
