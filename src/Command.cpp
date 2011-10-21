///////////////////////////////////////////////////////////////////////////////
// BOSSA
//
// Copyright (C) 2011 ShumaTech http://www.shumatech.com/
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
///////////////////////////////////////////////////////////////////////////////
#define __STDC_LIMIT_MACROS
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "Command.h"
#include "arm-dis.h"

#define min(a, b)   ((a) < (b) ? (a) : (b))

using namespace std;

Shell* Command::_shell = NULL;
Samba Command::_samba;
PortFactory Command::_portFactory;
FlashFactory Command::_flashFactory;
Flash::Ptr Command::_flash;
Flasher Command::_flasher(_flash);
bool Command::_connected = false;

Command::Command(const char* name, const char* help, const char* usage) :
    _name(name), _help(help), _usage(usage)
{
    assert(_shell != NULL);
}

bool
Command::error(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf(".  Try \"help %s\".\n", _name);

    return false;
}

bool
Command::argNum(int argc, int num)
{
    if (argc != num)
        return error("Command requires %d argument%s",
                     num - 1, (num > 1) ? "s" : "", _name);

    return true;
}

bool
Command::argRange(int argc, int min, int max)
{
    if (argc < min || argc > max)
        return error("Command requires %d to %d arguments", min - 1, max - 1);

    return true;
}

bool
Command::argUint32(const char* arg, uint32_t* addr)
{
    long long value;
    char *end;

    errno = 0;
    value = strtoll(arg, &end, 0);
    if (errno != 0 || *end != '\0')
        return error("Invalid number \"%s\"", arg);
    if (value < 0 || value > UINT32_MAX)
        return error("Number \"%s\" is out of range", arg);

    *addr = value;

    return true;
}

bool
Command::connected()
{
    if (!_connected)
    {
        printf("No device connected.  Use \"port\" or \"scan\" first.\n");
        return false;
    }
    return true;
}

bool
Command::flashable()
{
    if (!connected())
        return false;

    if (_flash.get() == NULL)
    {
        printf("Flash on device is not supported.\n");
        return false;
    }
    return true;
}

bool
Command::createFlash()
{
    uint32_t chipId = _samba.chipId();

    _flash = _flashFactory.create(_samba, chipId);
    if (_flash.get() == NULL)
    {
        printf("Flash for chip ID %08x is not supported\n", chipId);
        return false;
    }

    return true;
}

void
Command::hexdump(uint32_t addr, uint8_t *buf, size_t count)
{
    int lpad;
    int rpad;
    size_t size;
    size_t offset;
    const uint32_t ROW_SIZE = 16;
    const uint32_t ROW_MASK = ~(ROW_SIZE - 1);

    printf("            0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    while (count > 0)
    {
        lpad = (addr % ROW_SIZE);
        rpad = ROW_SIZE - min(lpad + count, ROW_SIZE);
        size = ROW_SIZE - rpad - lpad;

        printf("%08x | ", addr & ROW_MASK);

        printf("%*s", 3 * lpad, "");
        for (offset = 0; offset < size; offset++)
            printf("%02x ", buf[offset]);
        printf("%*s", 3 * rpad, "");

        printf("| ");

        printf("%*s", lpad, "");
        for (offset = 0; offset < size; offset++)
            printf("%c", isprint(buf[offset]) ? buf[offset] : '.');
        printf("%*s", rpad, "");

        printf("\n");

        buf += size;
        addr += size;
        count -= size;
    }
}

const char*
Command::binstr(uint32_t value, int bits)
{
    static char buf[36];
    char *str = buf;

    assert(bits <= 32 && bits > 0);

    for (int bitnum = bits - 1; bitnum >= 0; bitnum--)
    {
        *str++ = (value & (1 << bitnum)) ? '1' : '0';
        if (bitnum % 8 == 0)
            *str++ = ' ';
    }
    *(str - 1) = '\0';

    return buf;
}

void
Command::disconnect()
{
    _connected = false;
}

bool
Command::operator < (const Command& rhs)
{
    return (strcmp(_name, rhs._name) == -1);
}

CommandConnect::CommandConnect() :
    Command("connect",
            "Connect to device over serial port.",
            "port [SERIAL]\n"
            "  SERIAL -- host-specific serial port")
{}

void
CommandConnect::invoke(char* argv[], int argc)
{
    if (!argNum(argc, 2))
        return;

    if (!_samba.connect(_portFactory.create(argv[1])))
    {
        printf("No device found on %s\n", argv[1]);
        _connected = false;
        return;
    }

    printf("Connected to device on %s\n", argv[1]);
    _connected = true;
    createFlash();
}

CommandDebug::CommandDebug() :
    Command("debug",
            "Change the debug state.",
            "debug [STATE]\n"
            "  STATE - either \"off\" or \"on\"")
{}

void
CommandDebug::invoke(char* argv[], int argc)
{
    bool state;

    if (!argNum(argc, 2))
        return;

    if (strcasecmp(argv[1], "off") == 0)
        state = false;
    else if (strcasecmp(argv[1], "on") == 0)
        state = true;
    else
    {
        error("Invalid debug state - must be \"off\" or \"on\"");
        return;
    }

    _samba.setDebug(state);
}

CommandDisass::CommandDisass() :
    Command("disass",
            "Disassemble ARM code at memory address.",
            "disass [ADDRESS] [COUNT]\n"
            "  ADDRESS -- starting memory address, thumb mode if not word aligned\n"
            "  COUNT -- count of bytes to disassemble")
{}

void
CommandDisass::invoke(char* argv[], int argc)
{
    uint32_t addr;
    uint32_t count;
    uint8_t* buf;

    if (!argNum(argc, 3) ||
        !argUint32(argv[1], &addr) ||
        !argUint32(argv[2], &count) ||
        !connected())
        return;

    buf = (uint8_t*) malloc(count);
    if (!buf)
        return;

    for (uint32_t i = 0; i < count; i++)
        buf[i] = i;

    try
    {
        _samba.read(addr & ~0x1, buf, count);
    }
    catch (...)
    {
        free(buf);
        throw;
    }

    arm_dis_buf(buf, count, addr & ~0x1, addr & 0x3, 1);
    free(buf);
}

CommandDump::CommandDump() :
    Command("dump",
            "Dump memory in hexadecimal and ascii.",
            "dump [ADDRESS] [COUNT]\n"
            "  ADDRESS -- starting memory address\n"
            "  COUNT -- count of bytes to display")
{}

void
CommandDump::invoke(char* argv[], int argc)
{
    uint32_t addr;
    uint32_t count;
    uint8_t* buf;

    if (!argNum(argc, 3) ||
        !argUint32(argv[1], &addr) ||
        !argUint32(argv[2], &count) ||
        !connected())
        return;

    buf = (uint8_t*) malloc(count);
    if (!buf)
        return;

    try
    {
        _samba.read(addr, buf, count);
    }
    catch (...)
    {
        free(buf);
        throw;
    }

    hexdump(addr, buf, count);
    free(buf);
}

CommandErase::CommandErase() :
    Command("erase",
            "Erase the entire flash.",
            "erase")
{}

void
CommandErase::invoke(char* argv[], int argc)
{
    if (!argNum(argc, 1) ||
        !flashable())
        return;

    _flasher.erase();
}

CommandExit::CommandExit() :
    Command("exit",
            "Exit the BOSSA shell.",
            "exit")
{}

void
CommandExit::invoke(char* argv[], int argc)
{
    if (!argNum(argc, 1))
        return;

    _shell->exitFlag() = true;
}

CommandGo::CommandGo() :
    Command("go",
            "Execute ARM code at address.",
            "go [ADDRESS]\n"
            "  ADDRESS -- starting memory address of code to execute")
{}

void
CommandGo::invoke(char* argv[], int argc)
{
    uint32_t addr;

    if (!argNum(argc, 2) ||
        !argUint32(argv[1], &addr) ||
        !connected())
        return;

    _samba.go(addr);
}

CommandHelp::CommandHelp() :
    Command("help",
            "Display help for a command.",
            "help <COMMAND>\n"
            "  COMMAND -- (optional) display detailed usage for this command,\n"
            "             display summary help for all commands if not given")
{}

void
CommandHelp::invoke(char* argv[], int argc)
{
    if (!argRange(argc, 1, 2))
        return;

    if (argc == 1)
        _shell->help();
    else
        _shell->usage(argv[1]);
}

CommandHistory::CommandHistory() :
    Command("history",
            "List the command history.",
            "history")
{}

void
CommandHistory::invoke(char* argv[], int argc)
{
    if (!argNum(argc, 1))
        return;

    HIST_ENTRY **the_list = history_list ();
    if (the_list)
    for (int i = 0; the_list[i]; i++)
        printf ("  %d  %s\n", i + history_base, the_list[i]->line);
}

CommandInfo::CommandInfo() :
    Command("info",
            "Display information about the flash.",
            "info")
{}

void
CommandInfo::invoke(char* argv[], int argc)
{
    if (!argNum(argc, 1) ||
        !flashable())
        return;

    _flasher.info(_samba);
}

CommandLock::CommandLock() :
    Command("lock",
            "Set lock bits in the flash.",
            "lock <BITS>"
            "  BITS -- (optional) comma separated list of bits,"
            "          all bits if not given\n")
{}

void
CommandLock::invoke(char* argv[], int argc)
{
    string bits;

    if (!flashable())
        return;

    for (int argn = 1; argn < argc; argn++)
        bits += argv[argn];

    _flasher.lock(bits, true);
}

CommandMrb::CommandMrb() :
    Command("mrb",
            "Read bytes from memory.",
            "mrb [ADDRESS] <COUNT>\n"
            "  ADDRESS -- starting memory address\n"
            "  COUNT -- (optional) count of bytes to display, 1 if not given")
{}

void
CommandMrb::invoke(char* argv[], int argc)
{
    uint32_t addr;
    uint32_t count = 1;
    uint8_t value;

    if (!argRange(argc, 2, 3) ||
        !argUint32(argv[1], &addr) ||
        (argc == 3 && !argUint32(argv[2], &count)) ||
        !connected())
        return;

    while (count > 0)
    {
        value = _samba.readByte(addr);
        printf("%08x : %02x  %s\n", addr, value, binstr(value, 8));
        addr++;
        count--;
    }
}

CommandMrf::CommandMrf() :
    Command("mrf",
            "Read memory to file.",
            "mrf [ADDRESS] [COUNT] [FILE]\n"
            "  ADDRESS -- memory address to read\n"
            "  COUNT -- count of bytes to read\n"
            "  FILE -- file name on host filesystem to write")
{}

void
CommandMrf::invoke(char* argv[], int argc)
{
    uint32_t addr;
    uint32_t count;
    FILE* infile;
    uint8_t buf[1024];
    ssize_t fbytes;
    
    if (!argNum(argc, 4) ||
        !argUint32(argv[1], &addr) ||
        !argUint32(argv[2], &count) ||
        !connected())
        return;

    infile = fopen(argv[3], "wb");
    if (!infile)
        throw FileOpenError(errno);

    try
    {
        while (count > 0)
        {
            fbytes = min(count, sizeof(buf));
            _samba.read(addr, buf, fbytes);
            fbytes = fwrite(buf, 1, fbytes, infile);
            if (fbytes < 0)
                throw FileIoError(errno);
            if (fbytes != min(count, sizeof(buf)))
                throw FileShortError();
            count -= fbytes;
        }
    }
    catch (...)
    {
        fclose(infile);
        throw;
    }
    
    fclose(infile);
}

CommandMrw::CommandMrw() :
    Command("mrw",
            "Read words from memory.",
            "mrw [ADDRESS] <COUNT>\n"
            "  ADDRESS -- starting memory address\n"
            "  COUNT -- (optional) count of words to display, 1 if not given")
{}

void
CommandMrw::invoke(char* argv[], int argc)
{
    uint32_t addr;
    uint32_t count = 1;
    uint32_t value;

    if (!argRange(argc, 2, 3) ||
        !argUint32(argv[1], &addr) ||
        (argc == 3 && !argUint32(argv[2], &count)) ||
        !connected())
        return;

    while (count > 0)
    {
        value = _samba.readWord(addr);
        printf("%08x : %08x  %s\n", addr, value, binstr(value, 32));
        addr += 4;
        count--;
    }
}

CommandMwb::CommandMwb() :
    Command("mwb",
            "Write bytes to memory.",
            "mwb [ADDRESS] <VALUE>\n"
            "  ADDRESS -- starting memory address\n"
            "  VALUE -- (optional) value of byte to write, if not given"
            "           command will repeatedly prompt for input")
{}

void
CommandMwb::invoke(char* argv[], int argc)
{
    uint32_t addr;
    uint32_t value;

    if (!argRange(argc, 2, 3) ||
        !argUint32(argv[1], &addr) ||
        (argc == 3 && !argUint32(argv[2], &value)) ||
        !connected())
        return;

    do
    {
        if (argc == 2)
        {
            char* input = readline("? ");
            if (!input)
                return;
            if (input == '\0' ||
                !argUint32(input, &value))
            {
                free(input);
                return;
            }
            free(input);
        }

        if (value > 255)
        {
            error("Value out of range");
            return;
        }

        _samba.writeByte(addr, value);
        printf("%08x : %02x\n", addr, value);
        addr++;
    } while (argc == 2);
}

CommandMwf::CommandMwf() :
    Command("mwf",
            "Write memory from file.",
            "mwf [ADDRESS] [FILE]\n"
            "  ADDRESS -- memory address to write\n"
            "  FILE -- file name on host filesystem to read")
{}

void
CommandMwf::invoke(char* argv[], int argc)
{
    uint32_t addr;
    FILE* infile;
    uint8_t buf[1024];
    ssize_t fsize;
    ssize_t fbytes;
    ssize_t fpos;

    if (!argNum(argc, 3) ||
        !argUint32(argv[1], &addr) ||
        !connected())
        return;
    
    infile = fopen(argv[2], "rb");
    if (!infile)
        throw FileOpenError(errno);
    
    try
    {
        if (fseek(infile, 0, SEEK_END) != 0 ||
            (fsize = ftell(infile)) < 0)
            throw FileIoError(errno);

        rewind(infile);
            
        for (fpos = 0; fpos < fsize; fpos += fbytes)
        {
            fbytes = fread(buf, 1, min((size_t)fsize, sizeof(buf)), infile);
            if (fbytes < 0)
                throw FileIoError(errno);
            if (fbytes == 0)
                break;
            _samba.write(addr, buf, fbytes);
        }
    }
    catch (...)
    {
        fclose(infile);
        throw;
    }
    fclose(infile);
    printf("Wrote %ld bytes to address %08x\n", fsize, addr);
}

CommandMww::CommandMww() :
    Command("mww",
            "Write words to memory.",
            "mww [ADDRESS] <VALUE>\n"
            "  ADDRESS -- starting memory address\n"
            "  VALUE -- (optional) value of word to write, if not given"
            "           command will repeatedly prompt for input")
{}

void
CommandMww::invoke(char* argv[], int argc)
{
    uint32_t addr;
    uint32_t value;

    if (!argRange(argc, 2, 3) ||
        !argUint32(argv[1], &addr) ||
        (argc == 3 && !argUint32(argv[2], &value)) ||
        !connected())
        return;

    do
    {
        if (argc == 2)
        {
            char* input = readline("? ");
            if (!input)
                return;
            if (input == '\0' ||
                !argUint32(input, &value))
            {
                free(input);
                return;
            }
            free(input);
        }

        _samba.writeWord(addr, value);
        printf("%08x : %08x\n", addr, value);
        addr++;
    } while (argc == 2);
}

CommandPio::CommandPio() :
    Command("pio",
            "Parallel input/output operations.",
            "pio [LINE] [OPERATION]\n"
            "  LINE -- PIO line name (i.e. pa28, pc5, etc.)\n"
            "  OPERATION -- operation to perform on the PIO line.  One of the following:\n"
            "    detail -- detail about the line\n"
            "    high -- drive the output high\n"
            "    low -- drive the output low\n"
            "    status -- read the input status\n"
            "    input -- make the line an input"
            )
{}

void
CommandPio::invoke(char* argv[], int argc)
{
    uint32_t line;
    uint32_t chipId;
    uint32_t eproc;
    uint32_t arch;
    uint32_t addr = 0;
    size_t len;
    char port;
    
    if (!argNum(argc, 3) ||
        !connected())
        return;

    if (strlen(argv[1]) < 3 ||
        tolower(argv[1][0]) != 'p')
    {
        error("Invalid PIO line name");
        return;
    }
    
    if (!argUint32(&argv[1][2], &line))
        return;
        
    if (line >= 32)
    {
        error("Invalid PIO line number");
        return;
    }
    
    line = (1 << line);
    port = tolower(argv[1][1]);
    
    chipId = _samba.chipId();
    eproc = (chipId >> 5) & 0x7;
    arch = (chipId >> 20) & 0xff;
    
    // Check for Cortex-M3 register set
    if (eproc == 3)
    {
        // Check for SAM3U special case
        if (arch >= 0x80 && arch <= 0x81)
        {
            switch (port)
            {
                case 'a': addr = 0x400e0c00; break;
                case 'b': addr = 0x400e0e00; break;
                case 'c': addr = 0x400e1000; break;
            }
        }
        else
        {
            switch (port)
            {
                case 'a': addr = 0x400e0e00; break;
                case 'b': addr = 0x400e1000; break;
                case 'c': addr = 0x400e1200; break;
            }
        }
    }
    else
    {
        switch (port)
        {
            case 'a': addr = 0xfffff400; break;
            case 'b': addr = 0xfffff600; break;
            case 'c': addr = 0xfffff800; break;
        }
    }
    
    if (addr == 0)
    {
        printf("Invalid PIO line name\n");
        return;
    }
    
    static const uint32_t PIO_PER = 0x0;
    static const uint32_t PIO_PSR = 0x8;
    static const uint32_t PIO_OER = 0x10;
    static const uint32_t PIO_ODR = 0x14;
    static const uint32_t PIO_OSR = 0x18;
    static const uint32_t PIO_SODR = 0x30;
    static const uint32_t PIO_CODR = 0x34;
    static const uint32_t PIO_ODSR = 0x38;
    static const uint32_t PIO_PDSR = 0x3c;
    static const uint32_t PIO_ABSR = 0x70;
    
    len = strlen(argv[2]);
    if (strncasecmp(argv[2], "detail", len) == 0)
    {
        uint32_t data = _samba.readWord(addr + PIO_PSR);
        printf("PIO Status    : %s\n", (data & line) ? "PIO" : "periph");
        if (data & line)
        {
            data = _samba.readWord(addr + PIO_OSR);
            printf("Output Status : %s\n", (data & line) ? "output" : "input");
            if (data & line)
            {
                data = _samba.readWord(addr + PIO_ODSR);
                printf("Output Data   : %s\n", (data & line)? "high" : "low");
            }
            data = _samba.readWord(addr + PIO_PDSR);
            printf("Pin Data      : %s\n", (data & line)? "high" : "low");
        }
        else
        {
            data = _samba.readWord(addr + PIO_ABSR);
            printf("Periph Select : %s\n", (data & line) ? "A" : "B");
        }
    }
    else if (strncasecmp(argv[2], "high", len) == 0)
    {
        _samba.writeWord(addr + PIO_SODR, line);
        _samba.writeWord(addr + PIO_OER, line);
        _samba.writeWord(addr + PIO_PER, line);
    }
    else if (strncasecmp(argv[2], "low", len) == 0)
    {
        _samba.writeWord(addr + PIO_CODR, line);
        _samba.writeWord(addr + PIO_OER, line);
        _samba.writeWord(addr + PIO_PER, line);
    }
    else if (strncasecmp(argv[2], "status", len) == 0)
    {
        uint32_t data = _samba.readWord(addr +  PIO_PDSR);
        printf("%s\n", (data & line) ? "high" : "low");
    }
    else if (strncasecmp(argv[2], "input", len) == 0)
    {
        _samba.writeWord(addr + PIO_ODR, line);
        _samba.writeWord(addr + PIO_PER, line);
    }
    else
    {
        printf("Invalid PIO operation\n");
        return;
    }
}

CommandRead::CommandRead() :
    Command("read",
            "Read flash into a binary file.",
            "read [FILE] <COUNT>\n"
            "  FILE -- file name on host filesystem"
            "  COUNT -- (optional) count of bytes to read, defaults\n"
            "           to entire flash if not given")
{}

void
CommandRead::invoke(char* argv[], int argc)
{
    uint32_t count = 0;

    if (!argRange(argc, 2, 3) ||
        (argc == 3 && !argUint32(argv[2], &count)) ||
        !flashable())
        return;

    _flasher.read(argv[1], count);
}

CommandScan::CommandScan() :
    Command("scan",
            "Scan all serial ports for a device.",
            "scan")
{}

void
CommandScan::invoke(char* argv[], int argc)
{
    string port;

    if (!argNum(argc, 1))
        return;

    for (port = _portFactory.begin();
         port != _portFactory.end();
         port = _portFactory.next())
    {
        if (_samba.connect(_portFactory.create(port)))
        {
            printf("Device found on %s\n", port.c_str());
            _connected = true;
            createFlash();
            return;
        }
    }

    _connected = false;

    printf("Auto scan for device failed.\n"
           "Try specifying a serial port with the \"port\" command.\n");
}

CommandUnlock::CommandUnlock() :
    Command("unlock",
            "Clear lock bits in the flash.",
            "unlock <BITS>"
            "  BITS -- (optional) comma separated list of bits,"
            "          all bits if not given\n")
{}

void
CommandUnlock::invoke(char* argv[], int argc)
{
    string bits;

    if (!flashable())
        return;

    for (int argn = 1; argn < argc; argn++)
        bits += argv[argn];

    _flasher.lock(bits, true);
}

CommandVerify::CommandVerify() :
    Command("verify",
            "Verify binary file with the flash.",
            "verify [FILE]\n"
            "  FILE -- file name on host filesystem")
{}

void
CommandVerify::invoke(char* argv[], int argc)
{
    if (!argNum(argc, 2) ||
        !flashable())
        return;

    _flasher.verify(argv[1]);
}

CommandWrite::CommandWrite() :
    Command("write",
            "Write binary file into flash.",
            "write [FILE]\n"
            "  FILE -- file name on host filesystem")
{}

void
CommandWrite::invoke(char* argv[], int argc)
{
    if (!argNum(argc, 2) ||
        !flashable())
        return;

    _flasher.write(argv[1]);
}