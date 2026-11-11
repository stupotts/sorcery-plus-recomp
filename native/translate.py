"""Translate the recovered Sorcery+ Z80 code to native-compiled C before launch.

The generated cases bind each address to one instruction. Operand bytes remain
in RAM because the game patches call targets and constants. A changed opcode
traps; there is no runtime instruction decoder.
"""
import argparse
from pathlib import Path


class Instruction:
    def __init__(self, image, offset):
        self.image = image
        self.offset = offset
        self.length = 0
        self.opcodes = []
        self.index = ''
        self.displacement = None

    def take(self, count=1):
        first = self.length
        self.length += count
        return f'rd(pc+{first})' if count == 1 else f'word(pc+{first})'

    def opcode(self):
        value = self.image[self.offset + self.length]
        self.opcodes.append((self.length, value))
        self.length += 1
        return value

    def indexed_address(self):
        if not self.index:
            return 'HL'
        if self.displacement is None:
            self.displacement = self.take()
        return f'{self.index}+(int8_t){self.displacement}'

    def reg(self, code, plain=False):
        if code == 6:
            return f'RAM({self.indexed_address()})'
        if self.index and not plain and code in (4, 5):
            return self.index.lower() + ('h' if code == 4 else 'l')
        return 'bcdehl?a'[code]

    def pair(self, code, stack=False):
        return ('BC', 'DE', self.index or 'HL', 'AF' if stack else 'sp')[code]

    def set_pair(self, code, value, stack=False):
        pair = self.pair(code, stack)
        return f'{pair}={value};' if pair == 'sp' else f'SET({pair},{value});'

    def cb(self, op, address=None):
        group, sub, reg = op >> 6, (op >> 3) & 7, op & 7
        value = f'RAM({address})' if address else self.reg(reg)
        if group == 1:
            return f'bit_test({sub},{value});', 20 if address else (12 if reg == 6 else 8)
        expr = (f'rotate({sub},{value})', '', f'{value}&~(1u<<{sub})', f'{value}|(1u<<{sub})')[group]
        copy = f'{self.reg(reg, True)}={value};' if address and reg != 6 else ''
        return f'{value}={expr};{copy}', 23 if address else (15 if reg == 6 else 8)

    def ed(self, op):
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        pair, reverse = y // 2, y & 1
        if x == 1:
            if z == 0:
                assign = f'{self.reg(y)}=v;' if y != 6 else ''
                return f'uint8_t v=in_port(BC);{assign}f=(f&CY)|szp(v);', 12
            if z == 1:
                return f'out_port(BC,{self.reg(y) if y != 6 else "0"});', 12
            if z == 2:
                return f'SET(HL,adc16(HL,{self.pair(pair)},{1-reverse}));', 15
            if z == 3:
                address = self.take(2)
                if reverse:
                    return self.set_pair(pair, f'word({address})'), 20
                return f'put_word({address},{self.pair(pair)});', 20
            if z == 4:
                return 'a=sub8(0,a,0);', 8
            if z == 5:
                return 'iff=iff2;pc=pop();return 14;', 14
            if z == 6:
                return f'im={ (0,0,1,2,0,0,1,2)[y] };', 8
            return (
                'ir_i=a;', 'ir_r=a;', 'a=ir_i;f=(f&CY)|sz(a)|(iff2?PV:0);',
                'a=ir_r;f=(f&CY)|sz(a)|(iff2?PV:0);',
                'rld(0);', 'rld(1);', '', '',
            )[y], 18 if y in (4, 5) else 9
        if x == 2 and y >= 4 and z <= 3:
            return f'if(block_op({z},{-1 if y & 1 else 1}) && {int(y >= 6)}) return 21;', 16
        return '', 8

    def base(self, op):
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        p, q = y // 2, y & 1
        if x == 0:
            if z == 0:
                if y == 0:
                    return '', 4
                if y == 1:
                    return 'exchange_af();', 4
                test = '--b' if y == 2 else '1' if y == 3 else f'condition({y-4})'
                offset = self.take()
                return f'if({test}){{pc=NEXT+(int8_t){offset};return {13 if y == 2 else 12};}}', 8 if y == 2 else 7
            if z == 1:
                pair = self.pair(2)
                if q:
                    return f'SET({pair},add16({pair},{self.pair(p)}));', 11
                return self.set_pair(p, self.take(2)), 10
            if z == 2:
                if p < 2:
                    target = f'RAM({self.pair(p)})'
                    return (f'a={target};' if q else f'{target}=a;'), 7
                address = self.take(2)
                if p == 2:
                    return (self.set_pair(2, f'word({address})') if q else f'put_word({address},{self.pair(2)});'), 16
                return (f'a=RAM({address});' if q else f'RAM({address})=a;'), 13
            if z == 3:
                return self.set_pair(p, f'{self.pair(p)}{ "-" if q else "+" }1'), 6
            if z in (4, 5, 6):
                dst = self.reg(y)
                value = self.take() if z == 6 else f'incdec({dst},{int(z == 5)})'
                return f'{dst}={value};', (19 if self.index else 10 + (z != 6)) if y == 6 else (7 if z == 6 else 4)
            return ('acc_rotate(0);', 'acc_rotate(1);', 'acc_rotate(2);', 'acc_rotate(3);',
                    'decimal_adjust();', 'a=~a;f=(f&(SZP|CY))|HF|NF|(a&XY);',
                    'f=(f&SZP)|CY|(a&XY);', 'f=(f&SZP)|((f&CY)?HF:CY)|(a&XY);')[y], 4
        if x == 1:
            if y == z == 6:
                return 'halted=1;', 4
            indexed_memory = self.index and (y == 6 or z == 6)
            return f'{self.reg(y,indexed_memory)}={self.reg(z,indexed_memory)};', (15 if self.index else 7) if y == 6 or z == 6 else 4
        if x == 2:
            return f'alu({y},{self.reg(z)});', (15 if self.index else 7) if z == 6 else 4

        if z == 0:
            return f'if(condition({y})){{pc=pop();return 11;}}', 5
        if z == 1:
            if not q:
                return self.set_pair(p, 'pop()', True), 10
            return ('pc=pop();return 10;', 'exchange_all();', f'pc={self.pair(2)};return 4;', f'sp={self.pair(2)};')[p], 6
        if z == 2:
            target = self.take(2)
            return f'if(condition({y})){{pc={target};return 10;}}', 10
        if z == 3:
            if y == 0:
                return f'pc={self.take(2)};return 10;', 10
            if y in (2, 3):
                port = f'(a<<8)|{self.take()}'
                return (f'out_port({port},a);' if y == 2 else f'a=in_port({port});'), 11
            if y == 4:
                pair = self.pair(2)
                return f'uint16_t v=word(sp);put_word(sp,{pair});SET({pair},v);', 19
            return ('SET(DE,HL); SET(HL,old_de);', 'iff=iff2=0;', 'iff=iff2=1;irq_delay=2;')[y-5].replace('SET(DE,HL); SET(HL,old_de);','uint16_t v=DE;SET(DE,HL);SET(HL,v);'), 4
        if z == 4 or (z == 5 and q):
            target = self.take(2)
            call = f'push(NEXT);pc={target};return {17 + (4 if self.index else 0)};'
            return (f'if(condition({y})){{{call}}}' if z == 4 else call), 10
        if z == 5:
            return f'push({self.pair(p,True)});', 11
        if z == 6:
            return f'alu({y},{self.take()});', 7
        if y == 3:  # CPC RST 18h far call with inline descriptor pointer.
            return 'pc=NEXT;far_call();return 11;', 11
        return f'push(NEXT);pc={8*y};return 11;', 11

    def translate(self, address):
        op = self.opcode()
        prefixes = 0
        while op in (0xdd, 0xfd):
            self.index = 'IX' if op == 0xdd else 'IY'
            prefixes += 1
            op = self.opcode()
        if op == 0xcb:
            if self.index:
                mem = self.indexed_address()
                body, cycles = self.cb(self.opcode(), mem)
                cycles += 4*(prefixes-1)
            else:
                body, cycles = self.cb(self.opcode())
        elif op == 0xed:
            self.index = ''
            body, cycles = self.ed(self.opcode())
            cycles += 4*prefixes
        else:
            body, cycles = self.base(op)
            cycles += 4*prefixes
        guard = '||'.join(f'rd(pc+{i})!=0x{op:02x}' for i, op in self.opcodes)
        next_pc = f'0x{(address+self.length)&65535:04x}'
        body = body.replace('NEXT', f'({next_pc})')
        return f'case 0x{address:04x}:{{if({guard}) return changed_opcode(); {body} pc={next_pc};return {cycles};}}\n'


def generate(root, output):
    sources = [
        (0x7000, 'CODE-decoded-7000.bin'),
        (0x4f00, 'MUCODE-decoded-4f00.bin'),
    ]
    pages = {}
    for base, name in sources:
        image = (root / name).read_bytes()
        for offset in range(len(image)):
            address = base + offset
            try:
                instruction = Instruction(image, offset).translate(address)
            except IndexError:
                continue
            pages.setdefault(address >> 8, []).append(instruction)
    with output.open('w', encoding='ascii', newline='\n') as out:
        out.write('/* Generated from the supplied Sorcery+ code. Do not edit. */\n')
        for page, cases in sorted(pages.items()):
            out.write(f'static int page_{page:02x}(void){{switch(pc){{\n')
            out.writelines(cases)
            out.write('default:return unknown_address();}}\n')
        out.write('static int translated_step(void){switch(pc>>8){\n')
        for page in sorted(pages):
            out.write(f'case 0x{page:02x}:return page_{page:02x}();\n')
        out.write('default:return unknown_address();}}\n')


if __name__ == '__main__':
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('recovered', type=Path)
    args.add_argument('output', type=Path)
    values = args.parse_args()
    generate(values.recovered, values.output)
