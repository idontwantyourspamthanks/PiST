// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/M68kDisasm.h"

#include <QStringList>
#include <QVector>

namespace pist {

namespace {

QString dn(int n)
{
    return QStringLiteral("d%1").arg(n);
}

QString an(int n)
{
    return n == 7 ? QStringLiteral("sp") : QStringLiteral("a%1").arg(n);
}

QString sizeSuffix(int bytes)
{
    if (bytes == 1)
        return QStringLiteral(".b");
    if (bytes == 2)
        return QStringLiteral(".w");
    return QStringLiteral(".l");
}

QString hex(quint32 value)
{
    return QStringLiteral("$%1").arg(value, 0, 16);
}

QString named(quint32 value, const DisasmNames &names)
{
    const auto it = names.labels.constFind(value);
    if (it != names.labels.cend())
        return it.value();
    return hex(value);
}

QString immediateText(quint32 value, int sizeBytes, quint32 at, const DisasmNames &names,
                      QVector<quint32> *refs)
{
    if (sizeBytes == 4 && names.relocations.contains(at)) {
        if (refs)
            refs->append(value);
        const auto it = names.labels.constFind(value);
        if (it != names.labels.cend())
            return QStringLiteral("#") + it.value();
    }
    const auto eq = names.equates.constFind(value);
    if (eq != names.equates.cend())
        return QStringLiteral("#") + eq.value();
    return QStringLiteral("#") + hex(value);
}

const char *bccName(int cond)
{
    static const char *names[] = {
        "bra", "bsr", "bhi", "bls", "bcc", "bcs", "bne", "beq",
        "bvc", "bvs", "bpl", "bmi", "bge", "blt", "bgt", "ble"};
    return names[cond & 15];
}

const char *sccName(int cond)
{
    static const char *names[] = {
        "st", "sf", "shi", "sls", "scc", "scs", "sne", "seq",
        "svc", "svs", "spl", "smi", "sge", "slt", "sgt", "sle"};
    return names[cond & 15];
}

const char *dbName(int cond)
{
    static const char *names[] = {
        "dbt", "dbf", "dbhi", "dbls", "dbcc", "dbcs", "dbne", "dbeq",
        "dbvc", "dbvs", "dbpl", "dbmi", "dbge", "dblt", "dbgt", "dble"};
    return names[cond & 15];
}

const char *shiftName(int kind, bool left)
{
    static const char *names[4][2] = {
        {"asr", "asl"}, {"lsr", "lsl"}, {"roxr", "roxl"}, {"ror", "rol"}};
    return names[kind & 3][left ? 1 : 0];
}

quint16 reverseMask(quint16 mask)
{
    quint16 reversed = 0;
    for (int bit = 0; bit < 16; ++bit) {
        if (mask & (1u << bit))
            reversed |= quint16(1u << (15 - bit));
    }
    return reversed;
}

QString formatRegList(quint16 mask)
{
    QStringList parts;
    const auto emitRange = [&](int base, bool address) {
        for (int i = 0; i < 8;) {
            if ((mask & (1u << (base + i))) == 0) {
                ++i;
                continue;
            }
            int j = i;
            while (j + 1 < 8 && (mask & (1u << (base + j + 1))))
                ++j;
            const QString first = address ? QStringLiteral("a%1").arg(i) : dn(i);
            if (j == i)
                parts << first;
            else {
                const QString last = address ? QStringLiteral("a%1").arg(j) : dn(j);
                parts << first + QLatin1Char('-') + last;
            }
            i = j + 1;
        }
    };
    emitRange(0, false);
    emitRange(8, true);
    return parts.join(QLatin1Char('/'));
}

enum class EaKind { All, Data, DataNoImm, Alter, DataAlter, MemAlter, Control, MovemStore, MovemLoad };

bool eaAllowed(int mode, int reg, EaKind kind)
{
    if (mode == 7 && reg > 4)
        return false;
    const bool isAn = mode == 1;
    const bool isDn = mode == 0;
    const bool isImm = mode == 7 && reg == 4;
    const bool isPc = mode == 7 && (reg == 2 || reg == 3);
    const bool control = mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3);
    switch (kind) {
    case EaKind::All:
        return true;
    case EaKind::Data:
        return !isAn;
    case EaKind::DataNoImm:
        return !isAn && !isImm;
    case EaKind::Alter:
        return !isPc && !isImm;
    case EaKind::DataAlter:
        return !isAn && !isPc && !isImm;
    case EaKind::MemAlter:
        return !isDn && !isAn && !isPc && !isImm;
    case EaKind::Control:
        return control;
    case EaKind::MovemStore:
        return control || mode == 4;
    case EaKind::MovemLoad:
        return control || mode == 3;
    }
    return false;
}

struct Reader
{
    const QByteArray &image;
    quint32 limit = 0;
    quint32 pc = 0;
    bool ok = true;
    QVector<quint32> *refs = nullptr;

    quint16 word()
    {
        if (!ok || pc + 2 > limit || pc + 2 > quint32(image.size())) {
            ok = false;
            return 0;
        }
        const quint16 value = quint16((uchar(image.at(int(pc))) << 8) | uchar(image.at(int(pc + 1))));
        pc += 2;
        return value;
    }

    quint32 longWord()
    {
        const quint16 hi = word();
        const quint16 lo = word();
        return (quint32(hi) << 16) | lo;
    }
};

struct Ea
{
    bool ok = false;
    QString text;
    bool hasAddress = false;
    quint32 address = 0;
};

QString briefIndex(quint16 ext, const QString &base)
{
    const int ireg = (ext >> 12) & 7;
    const bool address = (ext & 0x8000) != 0;
    const bool wide = (ext & 0x0800) != 0;
    const int scale = (ext >> 9) & 3;
    const int disp = qint8(ext & 0xFF);
    QString index = address ? an(ireg) : dn(ireg);
    index += wide ? QStringLiteral(".l") : QStringLiteral(".w");
    if (scale)
        index += QStringLiteral("*%1").arg(1 << scale);
    return QStringLiteral("%1(%2,%3)").arg(disp).arg(base, index);
}

Ea readEa(Reader &reader, int mode, int reg, int sizeBytes, EaKind kind, const DisasmNames &names)
{
    Ea ea;
    if (!eaAllowed(mode, reg, kind))
        return ea;
    switch (mode) {
    case 0:
        ea.text = dn(reg);
        ea.ok = true;
        return ea;
    case 1:
        ea.text = an(reg);
        ea.ok = true;
        return ea;
    case 2:
        ea.text = QStringLiteral("(%1)").arg(an(reg));
        ea.ok = true;
        return ea;
    case 3:
        ea.text = QStringLiteral("(%1)+").arg(an(reg));
        ea.ok = true;
        return ea;
    case 4:
        ea.text = QStringLiteral("-(%1)").arg(an(reg));
        ea.ok = true;
        return ea;
    case 5: {
        const quint16 disp = reader.word();
        if (!reader.ok)
            return ea;
        ea.text = QStringLiteral("%1(%2)").arg(qint16(disp)).arg(an(reg));
        ea.ok = true;
        return ea;
    }
    case 6: {
        const quint16 ext = reader.word();
        if (!reader.ok)
            return ea;
        if (ext & 0x0100)
            return ea;
        ea.text = briefIndex(ext, an(reg));
        ea.ok = true;
        return ea;
    }
    case 7:
        break;
    default:
        return ea;
    }

    if (reg == 0 || reg == 1) {
        const bool wide = reg == 1;
        const quint32 raw = wide ? reader.longWord() : reader.word();
        if (!reader.ok)
            return ea;
        const quint32 address = wide ? raw : ((raw & 0x8000) ? (0xFFFF0000u | raw) : raw);
        ea.hasAddress = true;
        ea.address = address;
        if (reader.refs)
            reader.refs->append(address);
        const auto it = names.labels.constFind(address);
        if (it != names.labels.cend())
            ea.text = it.value();
        else
            ea.text = hex(wide ? raw : (raw & 0xFFFF)) + (wide ? QStringLiteral(".l") : QStringLiteral(".w"));
        ea.ok = true;
        return ea;
    }
    if (reg == 2) {
        const quint32 extAt = reader.pc;
        const quint16 disp = reader.word();
        if (!reader.ok)
            return ea;
        const quint32 target = extAt + quint32(qint32(qint16(disp)));
        ea.hasAddress = true;
        ea.address = target;
        if (reader.refs)
            reader.refs->append(target);
        const auto it = names.labels.constFind(target);
        ea.text = (it != names.labels.cend() ? it.value() : hex(target)) + QStringLiteral("(pc)");
        ea.ok = true;
        return ea;
    }
    if (reg == 3) {
        const quint16 ext = reader.word();
        if (!reader.ok)
            return ea;
        if (ext & 0x0100)
            return ea;
        ea.text = briefIndex(ext, QStringLiteral("pc"));
        ea.ok = true;
        return ea;
    }
    const quint32 at = reader.pc;
    quint32 value = 0;
    if (sizeBytes >= 4)
        value = reader.longWord();
    else {
        const quint16 word = reader.word();
        value = sizeBytes == 1 ? (word & 0xFF) : word;
    }
    if (!reader.ok)
        return ea;
    ea.text = immediateText(value, sizeBytes, at, names, reader.refs);
    ea.ok = true;
    return ea;
}

DecodedInsn asData(const QByteArray &image, quint32 start, quint32 end, bool falls)
{
    DecodedInsn insn;
    if (end < start + 2 || ((end - start) & 1u))
        return insn;
    QStringList words;
    for (quint32 at = start; at + 1 < end; at += 2) {
        const quint16 word = quint16((uchar(image.at(int(at))) << 8) | uchar(image.at(int(at + 1))));
        words << QStringLiteral("$%1").arg(word, 4, 16, QLatin1Char('0'));
    }
    insn.length = int(end - start);
    insn.text = QStringLiteral("dc.w ") + words.join(QLatin1Char(','));
    insn.flow = falls ? DecodedInsn::Flow::Fall : DecodedInsn::Flow::Stop;
    return insn;
}

class Decoder
{
public:
    Decoder(const QByteArray &image, quint32 address, quint32 limit, const DisasmNames &names)
        : image(image), names(names), reader{image, limit, address, true}, start(address)
    {
        reader.refs = &refs;
        op = reader.word();
    }

    DecodedInsn run()
    {
        if (!reader.ok)
            return {};
        if ((op & 0xF000) == 0xA000 || (op & 0xF000) == 0xF000)
            return asData(image, start, start + 2, true);
        if ((op >> 12) == 0)
            return group0();
        if ((op >> 12) >= 1 && (op >> 12) <= 3)
            return groupMove();
        if ((op >> 12) == 4)
            return group4();
        if ((op >> 12) == 5)
            return group5();
        if ((op >> 12) == 6)
            return group6();
        if ((op >> 12) == 7)
            return group7();
        if ((op >> 12) == 8)
            return groupOr();
        if ((op >> 12) == 9)
            return groupAddLike("sub", "subx", "suba");
        if ((op >> 12) == 0xB)
            return groupCmp();
        if ((op >> 12) == 0xC)
            return groupAnd();
        if ((op >> 12) == 0xD)
            return groupAddLike("add", "addx", "adda");
        if ((op >> 12) == 0xE)
            return groupShift();
        return data();
    }

private:
    const QByteArray &image;
    const DisasmNames &names;
    QVector<quint32> refs;
    Reader reader;
    quint32 start = 0;
    quint16 op = 0;

    int mode() const { return (op >> 3) & 7; }
    int eareg() const { return op & 7; }
    int reg9() const { return (op >> 9) & 7; }

    DecodedInsn data() const
    {
        if (!reader.ok)
            return {};
        return asData(image, start, reader.pc, false);
    }

    DecodedInsn done(const QString &text, DecodedInsn::Flow flow = DecodedInsn::Flow::Fall,
                     bool hasTarget = false, quint32 target = 0, bool indirect = false) const
    {
        DecodedInsn insn;
        insn.length = int(reader.pc - start);
        insn.text = text;
        insn.flow = flow;
        insn.hasTarget = hasTarget;
        insn.target = target;
        insn.indirect = indirect;
        if (reader.refs)
            insn.refs = *reader.refs;
        return insn;
    }

    DecodedInsn jump(const char *mnemonic, DecodedInsn::Flow flow)
    {
        const Ea ea = readEa(reader, mode(), eareg(), 4, EaKind::Control, names);
        if (!reader.ok)
            return {};
        if (!ea.ok)
            return data();
        const bool indirect = !ea.hasAddress;
        const auto taken = (!indirect && flow == DecodedInsn::Flow::Branch)
                               ? DecodedInsn::Flow::Branch
                               : flow;
        const auto actual = indirect && flow == DecodedInsn::Flow::Branch
                                ? DecodedInsn::Flow::Stop
                                : taken;
        return done(QStringLiteral("%1 %2").arg(QLatin1String(mnemonic), ea.text), actual,
                    ea.hasAddress, ea.address, indirect);
    }

    DecodedInsn group0()
    {
        if ((op & 0xFF00) == 0x0800) {
            static const char *bitNames[] = {"btst", "bchg", "bclr", "bset"};
            const int kind = (op >> 6) & 3;
            const quint16 bitWord = reader.word();
            if (!reader.ok)
                return {};
            const int bit = bitWord & (mode() == 0 ? 31 : 7);
            const Ea ea = readEa(reader, mode(), eareg(), 1,
                                 kind == 0 ? EaKind::DataNoImm : EaKind::DataAlter, this->names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1 #%2,%3").arg(QLatin1String(bitNames[kind])).arg(bit).arg(ea.text));
        }
        if ((op & 0xF100) == 0x0100) {
            if (mode() == 1) {
                const int form = (op >> 6) & 3;
                const quint16 disp = reader.word();
                if (!reader.ok)
                    return {};
                const QString mem = QStringLiteral("%1(%2)").arg(qint16(disp)).arg(an(eareg()));
                const QString dataReg = dn(reg9());
                const QString suffix = (form & 1) ? QStringLiteral(".l") : QStringLiteral(".w");
                const bool toMem = form >= 2;
                return done(toMem ? QStringLiteral("movep%1 %2,%3").arg(suffix, dataReg, mem)
                                  : QStringLiteral("movep%1 %2,%3").arg(suffix, mem, dataReg));
            }
            static const char *bitNames[] = {"btst", "bchg", "bclr", "bset"};
            const int kind = (op >> 6) & 3;
            const Ea ea = readEa(reader, mode(), eareg(), 1,
                                 kind == 0 ? EaKind::DataNoImm : EaKind::DataAlter, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1 %2,%3").arg(QLatin1String(bitNames[kind]), dn(reg9()), ea.text));
        }
        if ((op & 0xF100) != 0x0000)
            return data();
        const int operation = (op >> 9) & 7;
        const int size = (op >> 6) & 3;
        static const char *immediates[] = {"ori", "andi", "subi", "addi", nullptr, "eori", "cmpi", nullptr};
        if (operation > 7 || immediates[operation] == nullptr || size == 3)
            return data();
        const int bytes = 1 << size;
        const quint32 at = reader.pc;
        const quint32 value = bytes >= 4 ? reader.longWord()
                                         : (bytes == 1 ? (reader.word() & 0xFF) : reader.word());
        if (!reader.ok)
            return {};
        const QString imm = immediateText(value, bytes, at, names, reader.refs);
        const bool special = mode() == 7 && eareg() == 4;
        const bool toStatus = special && (operation == 0 || operation == 1 || operation == 5);
        if (toStatus && size == 0)
            return done(QStringLiteral("%1.b %2,ccr").arg(QLatin1String(immediates[operation]), imm));
        if (toStatus && size == 1)
            return done(QStringLiteral("%1.w %2,sr").arg(QLatin1String(immediates[operation]), imm));
        if (special)
            return data();
        const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::DataAlter, names);
        if (!reader.ok)
            return {};
        if (!ea.ok)
            return data();
        return done(QStringLiteral("%1%2 %3,%4")
                        .arg(QLatin1String(immediates[operation]), sizeSuffix(bytes), imm, ea.text));
    }

    DecodedInsn groupMove()
    {
        const int code = (op >> 12) & 3;
        const int bytes = code == 1 ? 1 : code == 2 ? 4 : 2;
        const int destReg = reg9();
        const int destMode = (op >> 6) & 7;
        const Ea src = readEa(reader, mode(), eareg(), bytes, EaKind::All, names);
        if (!reader.ok)
            return {};
        if (!src.ok)
            return data();
        if (destMode == 1) {
            if (bytes == 1)
                return data();
            return done(QStringLiteral("movea%1 %2,%3").arg(sizeSuffix(bytes), src.text, an(destReg)));
        }
        const Ea dst = readEa(reader, destMode, destReg, bytes,
                              bytes == 1 ? EaKind::DataAlter : EaKind::Alter, names);
        if (!reader.ok)
            return {};
        if (!dst.ok)
            return data();
        return done(QStringLiteral("move%1 %2,%3").arg(sizeSuffix(bytes), src.text, dst.text));
    }

    DecodedInsn group4()
    {
        if (op == 0x4E70)
            return done(QStringLiteral("reset"));
        if (op == 0x4E71)
            return done(QStringLiteral("nop"));
        if (op == 0x4E72) {
            const quint16 sr = reader.word();
            if (!reader.ok)
                return {};
            return done(QStringLiteral("stop #$%1").arg(sr, 0, 16));
        }
        if (op == 0x4E73)
            return done(QStringLiteral("rte"), DecodedInsn::Flow::Stop);
        if (op == 0x4E75)
            return done(QStringLiteral("rts"), DecodedInsn::Flow::Stop);
        if (op == 0x4E76)
            return done(QStringLiteral("trapv"));
        if (op == 0x4E77)
            return done(QStringLiteral("rtr"), DecodedInsn::Flow::Stop);
        if (op == 0x4AFC)
            return done(QStringLiteral("illegal"), DecodedInsn::Flow::Stop);
        if ((op & 0xFFF0) == 0x4E40)
            return done(QStringLiteral("trap #%1").arg(op & 0xF));
        if ((op & 0xFFF8) == 0x4E50) {
            const quint16 disp = reader.word();
            if (!reader.ok)
                return {};
            return done(QStringLiteral("link %1,#%2").arg(an(op & 7)).arg(qint16(disp)));
        }
        if ((op & 0xFFF8) == 0x4E58)
            return done(QStringLiteral("unlk %1").arg(an(op & 7)));
        if ((op & 0xFFF8) == 0x4E60)
            return done(QStringLiteral("move.l %1,usp").arg(an(op & 7)));
        if ((op & 0xFFF8) == 0x4E68)
            return done(QStringLiteral("move.l usp,%1").arg(an(op & 7)));
        if ((op & 0xFFC0) == 0x4E80)
            return jump("jsr", DecodedInsn::Flow::Call);
        if ((op & 0xFFC0) == 0x4EC0)
            return jump("jmp", DecodedInsn::Flow::Branch);
        if ((op & 0xF1C0) == 0x4180) {
            const Ea ea = readEa(reader, mode(), eareg(), 2, EaKind::Data, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("chk.w %1,%2").arg(ea.text, dn(reg9())));
        }
        if ((op & 0xF1C0) == 0x41C0) {
            const Ea ea = readEa(reader, mode(), eareg(), 4, EaKind::Control, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("lea %1,%2").arg(ea.text, an(reg9())));
        }
        if ((op & 0xFFB8) == 0x4880)
            return done(QStringLiteral("ext%1 %2").arg((op & 0x40) ? QStringLiteral(".l") : QStringLiteral(".w"),
                                                       dn(op & 7)));
        if ((op & 0xFFF8) == 0x4840)
            return done(QStringLiteral("swap %1").arg(dn(op & 7)));
        if ((op & 0xFFC0) == 0x4840) {
            const Ea ea = readEa(reader, mode(), eareg(), 4, EaKind::Control, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("pea %1").arg(ea.text));
        }
        if ((op & 0xFFC0) == 0x4800) {
            const Ea ea = readEa(reader, mode(), eareg(), 1, EaKind::DataAlter, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("nbcd %1").arg(ea.text));
        }
        if ((op & 0xFB80) == 0x4880)
            return movem();
        if ((op & 0xFFC0) == 0x40C0)
            return statusMove(QStringLiteral("move.w sr,%1"), EaKind::DataAlter);
        if ((op & 0xFFC0) == 0x44C0)
            return statusMove(QStringLiteral("move.w %1,ccr"), EaKind::Data);
        if ((op & 0xFFC0) == 0x46C0)
            return statusMove(QStringLiteral("move.w %1,sr"), EaKind::Data);
        const quint16 family = op & 0xFF00;
        if (family == 0x4000 || family == 0x4200 || family == 0x4400 || family == 0x4600) {
            const int size = (op >> 6) & 3;
            if (size == 3)
                return data();
            const char *mnemonic = family == 0x4000 ? "negx" : family == 0x4200 ? "clr"
                                                               : family == 0x4400 ? "neg"
                                                                                  : "not";
            const int bytes = 1 << size;
            const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::DataAlter, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1%2 %3").arg(QLatin1String(mnemonic), sizeSuffix(bytes), ea.text));
        }
        if ((op & 0xFFC0) == 0x4AC0) {
            const Ea ea = readEa(reader, mode(), eareg(), 1, EaKind::DataAlter, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("tas %1").arg(ea.text));
        }
        if ((op & 0xFF00) == 0x4A00) {
            const int size = (op >> 6) & 3;
            if (size == 3)
                return data();
            const int bytes = 1 << size;
            const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::DataAlter, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("tst%1 %2").arg(sizeSuffix(bytes), ea.text));
        }
        return data();
    }

    DecodedInsn statusMove(const QString &pattern, EaKind kind)
    {
        const Ea ea = readEa(reader, mode(), eareg(), 2, kind, names);
        if (!reader.ok)
            return {};
        if (!ea.ok)
            return data();
        return done(pattern.arg(ea.text));
    }

    DecodedInsn movem()
    {
        const bool toReg = (op & 0x0400) != 0;
        const bool wide = (op & 0x0040) != 0;
        const quint16 maskWord = reader.word();
        if (!reader.ok)
            return {};
        const Ea ea = readEa(reader, mode(), eareg(), wide ? 4 : 2,
                             toReg ? EaKind::MovemLoad : EaKind::MovemStore, names);
        if (!reader.ok)
            return {};
        if (!ea.ok)
            return data();
        quint16 mask = maskWord;
        if (!toReg && mode() == 4)
            mask = reverseMask(mask);
        const QString list = formatRegList(mask);
        const QString suffix = wide ? QStringLiteral(".l") : QStringLiteral(".w");
        return done(toReg ? QStringLiteral("movem%1 %2,%3").arg(suffix, ea.text, list)
                          : QStringLiteral("movem%1 %2,%3").arg(suffix, list, ea.text));
    }

    DecodedInsn group5()
    {
        if ((op & 0xF0F8) == 0x50C8) {
            const quint16 disp = reader.word();
            if (!reader.ok)
                return {};
            const quint32 target = start + 2 + quint32(qint32(qint16(disp)));
            DecodedInsn insn = done(QStringLiteral("%1 %2,%3")
                                        .arg(QLatin1String(dbName((op >> 8) & 15)), dn(op & 7), named(target, names)),
                                    DecodedInsn::Flow::Call);
            insn.hasTarget = true;
            insn.target = target;
            return insn;
        }
        if (((op >> 6) & 3) == 3) {
            const Ea ea = readEa(reader, mode(), eareg(), 1, EaKind::DataAlter, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1 %2").arg(QLatin1String(sccName((op >> 8) & 15)), ea.text));
        }
        const int size = (op >> 6) & 3;
        const bool subtract = (op & 0x0100) != 0;
        int count = reg9();
        if (count == 0)
            count = 8;
        if (size == 0 && mode() == 1)
            return data();
        const Ea ea = readEa(reader, mode(), eareg(), 1 << size,
                             mode() == 1 ? EaKind::Alter : EaKind::DataAlter, names);
        if (!reader.ok)
            return {};
        if (!ea.ok)
            return data();
        return done(QStringLiteral("%1%2 #%3,%4")
                        .arg(subtract ? QStringLiteral("subq") : QStringLiteral("addq"), sizeSuffix(1 << size))
                        .arg(count)
                        .arg(ea.text));
    }

    DecodedInsn group6()
    {
        const int cond = (op >> 8) & 15;
        const int disp8 = op & 0xFF;
        qint32 disp = 0;
        QString suffix;
        if (disp8 == 0) {
            const quint16 word = reader.word();
            if (!reader.ok)
                return {};
            disp = qint16(word);
            suffix = QStringLiteral(".w");
        } else {
            disp = qint8(disp8);
            suffix = QStringLiteral(".s");
        }
        const quint32 target = start + 2 + quint32(disp);
        const auto flow = cond == 0 ? DecodedInsn::Flow::Branch : DecodedInsn::Flow::Call;
        DecodedInsn insn = done(QStringLiteral("%1%2 %3").arg(QLatin1String(bccName(cond)), suffix,
                                                              named(target, names)),
                                flow);
        insn.hasTarget = true;
        insn.target = target;
        return insn;
    }

    DecodedInsn group7()
    {
        if (op & 0x0100)
            return data();
        return done(QStringLiteral("moveq #%1,%2").arg(int(qint8(op & 0xFF))).arg(dn(reg9())));
    }

    DecodedInsn groupOr()
    {
        const int opmode = (op >> 6) & 7;
        const int ry = reg9();
        const int rx = eareg();
        if (opmode == 3 || opmode == 7) {
            const Ea ea = readEa(reader, mode(), eareg(), 2, EaKind::Data, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1 %2,%3")
                            .arg(opmode == 3 ? QStringLiteral("divu.w") : QStringLiteral("divs.w"), ea.text, dn(ry)));
        }
        if (opmode >= 4 && opmode <= 6 && ((op >> 4) & 3) == 0) {
            if (opmode != 4)
                return data();
            return pair("sbcd", QString(), ry, rx, (op & 8) != 0);
        }
        return registerMemory("or", opmode, ry);
    }

    DecodedInsn groupAnd()
    {
        const int opmode = (op >> 6) & 7;
        const int rx = reg9();
        const int ry = eareg();
        if (opmode == 5 && mode() == 0)
            return done(QStringLiteral("exg %1,%2").arg(dn(rx), dn(ry)));
        if (opmode == 5 && mode() == 1)
            return done(QStringLiteral("exg %1,%2").arg(an(rx), an(ry)));
        if (opmode == 6 && mode() == 1)
            return done(QStringLiteral("exg %1,%2").arg(dn(rx), an(ry)));
        if (opmode == 3 || opmode == 7) {
            const Ea ea = readEa(reader, mode(), eareg(), 2, EaKind::Data, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1 %2,%3")
                            .arg(opmode == 3 ? QStringLiteral("mulu.w") : QStringLiteral("muls.w"), ea.text, dn(rx)));
        }
        if (opmode >= 4 && opmode <= 6 && ((op >> 4) & 3) == 0) {
            if (opmode != 4)
                return data();
            return pair("abcd", QString(), rx, ry, (op & 8) != 0);
        }
        return registerMemory("and", opmode, rx);
    }

    DecodedInsn groupAddLike(const char *mnemonic, const char *extended, const char *address)
    {
        const int opmode = (op >> 6) & 7;
        const int ry = reg9();
        const int rx = eareg();
        if (opmode == 3 || opmode == 7) {
            const int bytes = opmode == 3 ? 2 : 4;
            const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::All, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1%2 %3,%4").arg(QLatin1String(address), sizeSuffix(bytes), ea.text, an(ry)));
        }
        if (opmode >= 4 && opmode <= 6 && ((op >> 4) & 3) == 0)
            return pair(extended, sizeSuffix(1 << (opmode - 4)), ry, rx, (op & 8) != 0);
        return registerMemory(mnemonic, opmode, ry);
    }

    DecodedInsn groupCmp()
    {
        const int opmode = (op >> 6) & 7;
        const int ry = reg9();
        const int rx = eareg();
        if (opmode == 3 || opmode == 7) {
            const int bytes = opmode == 3 ? 2 : 4;
            const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::All, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("cmpa%1 %2,%3").arg(sizeSuffix(bytes), ea.text, an(ry)));
        }
        if (opmode >= 4 && opmode <= 6 && mode() == 1) {
            const int bytes = 1 << (opmode - 4);
            return done(QStringLiteral("cmpm%1 (%2)+,(%3)+").arg(sizeSuffix(bytes), an(rx), an(ry)));
        }
        if (opmode <= 2) {
            const int bytes = 1 << opmode;
            const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::All, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("cmp%1 %2,%3").arg(sizeSuffix(bytes), ea.text, dn(ry)));
        }
        const int bytes = 1 << (opmode - 4);
        const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::DataAlter, names);
        if (!reader.ok)
            return {};
        if (!ea.ok)
            return data();
        return done(QStringLiteral("eor%1 %2,%3").arg(sizeSuffix(bytes), dn(ry), ea.text));
    }

    DecodedInsn pair(const char *mnemonic, const QString &suffix, int dest, int source, bool memory)
    {
        const QString src = memory ? QStringLiteral("-(%1)").arg(an(source)) : dn(source);
        const QString dst = memory ? QStringLiteral("-(%1)").arg(an(dest)) : dn(dest);
        return done(QStringLiteral("%1%2 %3,%4").arg(QLatin1String(mnemonic), suffix, src, dst));
    }

    DecodedInsn registerMemory(const char *mnemonic, int opmode, int dataReg)
    {
        if (opmode <= 2) {
            const int bytes = 1 << opmode;
            const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::Data, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1%2 %3,%4").arg(QLatin1String(mnemonic), sizeSuffix(bytes), ea.text, dn(dataReg)));
        }
        if (opmode >= 4 && opmode <= 6) {
            const int bytes = 1 << (opmode - 4);
            const Ea ea = readEa(reader, mode(), eareg(), bytes, EaKind::MemAlter, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1%2 %3,%4").arg(QLatin1String(mnemonic), sizeSuffix(bytes), dn(dataReg), ea.text));
        }
        return data();
    }

    DecodedInsn groupShift()
    {
        const int size = (op >> 6) & 3;
        if (size == 3) {
            if (op & 0x0800)
                return data();
            const char *mnemonic = shiftName((op >> 9) & 3, (op & 0x0100) != 0);
            const Ea ea = readEa(reader, mode(), eareg(), 2, EaKind::MemAlter, names);
            if (!reader.ok)
                return {};
            if (!ea.ok)
                return data();
            return done(QStringLiteral("%1.w %2").arg(QLatin1String(mnemonic), ea.text));
        }
        const char *mnemonic = shiftName((op >> 3) & 3, (op & 0x0100) != 0);
        const bool inReg = (op & 0x0020) != 0;
        const int countReg = reg9();
        const QString count = inReg ? dn(countReg)
                                    : QStringLiteral("#%1").arg(countReg == 0 ? 8 : countReg);
        return done(QStringLiteral("%1%2 %3,%4").arg(QLatin1String(mnemonic), sizeSuffix(1 << size), count, dn(op & 7)));
    }
};

} // namespace

DecodedInsn decode68k(const QByteArray &image, quint32 address, quint32 limit, const DisasmNames &names)
{
    if ((address & 1u) || address >= limit)
        return {};
    Decoder decoder(image, address, limit, names);
    return decoder.run();
}

} // namespace pist
