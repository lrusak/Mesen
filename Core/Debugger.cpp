#include "stdafx.h"
#include "../Utilities/FolderUtilities.h"
#include "MessageManager.h"
#include "Debugger.h"
#include "Console.h"
#include "BaseMapper.h"
#include "Disassembler.h"
#include "VideoDecoder.h"
#include "APU.h"
#include "SoundMixer.h"
#include "CodeDataLogger.h"
#include "LabelManager.h"
#include "MemoryDumper.h"
#include "MemoryAccessCounter.h"
#include "Assembler.h"
#include "DisassemblyInfo.h"
#include "PPU.h"
#include "MemoryManager.h"
#include "StandardController.h"
#include "CodeDataLogger.h"
#include "DummyCpu.h"

string Debugger::_disassemblerOutput = "";

Debugger::Debugger(shared_ptr<Console> console, shared_ptr<CPU> cpu, shared_ptr<PPU> ppu, shared_ptr<APU> apu, shared_ptr<MemoryManager> memoryManager, shared_ptr<BaseMapper> mapper)
{
	_romName = console->GetRomInfo().RomName;
	_console = console;
	_cpu = cpu;
	_apu = apu;
	_memoryManager = memoryManager;
	_mapper = mapper;

	_labelManager.reset(new LabelManager(_mapper));
	_disassembler.reset(new Disassembler(memoryManager.get(), mapper.get(), this));
	_codeDataLogger.reset(new CodeDataLogger(this, mapper->GetMemorySize(DebugMemoryType::PrgRom), mapper->GetMemorySize(DebugMemoryType::ChrRom)));

	SetPpu(ppu);

	_memoryAccessCounter.reset(new MemoryAccessCounter(this));

	_opCodeCycle = 0;

	_currentReadAddr = nullptr;
	_currentReadValue = nullptr;
	_nextReadAddr = -1;
	_returnToAddress = 0;

	_flags = 0;

	_prevInstructionCycle = -1;
	_curInstructionCycle = -1;

	_disassemblerOutput = "";
	
	memset(_inputOverride, 0, sizeof(_inputOverride));

	_disassembler->Reset();

	_released = false;
}

Debugger::~Debugger()
{
	if(!_released) {
		_released = true;
	}
}

void Debugger::SetPpu(shared_ptr<PPU> ppu)
{
	_ppu = ppu;
	_memoryDumper.reset(new MemoryDumper(_ppu, _memoryManager, _mapper, _codeDataLogger, this, _disassembler));
}

Console* Debugger::GetConsole()
{
	return _console.get();
}

void Debugger::SetFlags(uint32_t flags)
{
	bool needUpdate = ((flags ^ _flags) & (int)DebuggerFlags::DisplayOpCodesInLowerCase) != 0;
	_flags = flags;
	if(needUpdate) {
		_disassembler->BuildOpCodeTables(CheckFlag(DebuggerFlags::DisplayOpCodesInLowerCase));
	}
}

bool Debugger::CheckFlag(DebuggerFlags flag)
{
	return (_flags & (uint32_t)flag) == (uint32_t)flag;
}

bool Debugger::IsMarkedAsCode(uint16_t relativeAddress)
{
	AddressTypeInfo info;
	GetAbsoluteAddressAndType(relativeAddress, &info);
	if(info.Address >= 0 && info.Type == AddressType::PrgRom)
		return _codeDataLogger->IsCode(info.Address);
	return false;
}

shared_ptr<CodeDataLogger> Debugger::GetCodeDataLogger()
{
	return _codeDataLogger;
}

shared_ptr<LabelManager> Debugger::GetLabelManager()
{
	return _labelManager;
}

void Debugger::GetApuState(ApuState *state)
{
	//Force APU to catch up before we retrieve its state
	_apu->Run();

	*state = _apu->GetState();
}

void Debugger::GetState(DebugState *state, bool includeMapperInfo)
{
	state->Model = _console->GetModel();
	state->ClockRate = _cpu->GetClockRate(_console->GetModel());
	_cpu->GetState(state->CPU);
	_ppu->GetState(state->PPU);
	if(includeMapperInfo) {
		state->Cartridge = _mapper->GetState();
		state->APU = _apu->GetState();
	}
}

void Debugger::SetState(DebugState state)
{
	_cpu->SetState(state.CPU);
	_ppu->SetState(state.PPU);
	if(state.CPU.PC != _cpu->GetPC()) {
		SetNextStatement(state.CPU.PC);
	}
}

void Debugger::GenerateCodeOutput()
{
	State cpuState;
	_cpu->GetState(cpuState);

	_disassemblerOutput.clear();
	_disassemblerOutput.reserve(10000);

	for(uint32_t i = 0; i < 0x10000; i += 0x100) {
		//Merge all sequential ranges into 1 chunk
		AddressTypeInfo startInfo, currentInfo, endInfo;
		GetAbsoluteAddressAndType(i, &startInfo);
		currentInfo = startInfo;
		GetAbsoluteAddressAndType(i+0x100, &endInfo);

		uint32_t startMemoryAddr = i;
		int32_t startAddr, endAddr;

		if(startInfo.Address >= 0) {
			startAddr = startInfo.Address;
			endAddr = startAddr + 0xFF;
			while(currentInfo.Type == endInfo.Type && currentInfo.Address + 0x100 == endInfo.Address && i < 0x10000) {
				endAddr += 0x100;
				currentInfo = endInfo;
				i+=0x100;
				GetAbsoluteAddressAndType(i + 0x100, &endInfo);
			}
			_disassemblerOutput += _disassembler->GetCode(startInfo, endAddr, startMemoryAddr, cpuState, _memoryManager, _labelManager);
		}
	}
}

const char* Debugger::GetCode(uint32_t &length)
{
	string previousCode = _disassemblerOutput;
	GenerateCodeOutput();
	bool forceRefresh = length == (uint32_t)-1;
	length = (uint32_t)_disassemblerOutput.size();
	if(!forceRefresh && previousCode.compare(_disassemblerOutput) == 0)
		//Return null pointer if the code is identical to last call
		//This avoids the UTF8->UTF16 conversion that the UI 
                //needs to do
		//before comparing the strings
		return nullptr;
	return _disassemblerOutput.c_str();
}

int32_t Debugger::GetRelativeAddress(uint32_t addr, AddressType type)
{
	switch(type) {
		case AddressType::InternalRam: 
		case AddressType::Register:
			return addr;
		
		case AddressType::PrgRom: 
		case AddressType::WorkRam: 
		case AddressType::SaveRam: 
			return _mapper->FromAbsoluteAddress(addr, type);
	}

	return -1;
}

int32_t Debugger::GetRelativePpuAddress(uint32_t addr, PpuAddressType type)
{
	if(type == PpuAddressType::PaletteRam) {
		return 0x3F00 | (addr & 0x1F);
	}
	return _mapper->FromAbsolutePpuAddress(addr, type);
}

int32_t Debugger::GetAbsoluteAddress(uint32_t addr)
{
	return _mapper->ToAbsoluteAddress(addr);
}

int32_t Debugger::GetAbsoluteChrAddress(uint32_t addr)
{
	return _mapper->ToAbsoluteChrAddress(addr);
}

void Debugger::SetNextStatement(uint16_t addr)
{
	if(_currentReadAddr) {
		_cpu->SetDebugPC(addr);
		*_currentReadAddr = addr;
		*_currentReadValue = _memoryManager->DebugRead(addr, false);
	} else {
		//Can't change the address right away (CPU is in the middle of an instruction)
		//Address will change after current instruction is done executing
		_nextReadAddr = addr;
	}
}

shared_ptr<MemoryDumper> Debugger::GetMemoryDumper()
{
	return _memoryDumper;
}

void Debugger::GetAbsoluteAddressAndType(uint32_t relativeAddr, AddressTypeInfo* info)
{
	return _mapper->GetAbsoluteAddressAndType(relativeAddr, info);
}

void Debugger::GetPpuAbsoluteAddressAndType(uint32_t relativeAddr, PpuAddressTypeInfo* info)
{
	return _mapper->GetPpuAbsoluteAddressAndType(relativeAddr, info);
}

void Debugger::GetNesHeader(uint8_t* header)
{
	NESHeader nesHeader = _mapper->GetRomInfo().NesHeader;
	memcpy(header, &nesHeader, sizeof(NESHeader));
}
