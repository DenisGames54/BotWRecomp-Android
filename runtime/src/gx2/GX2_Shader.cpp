// Vendored from Cemu (src/Cafe/OS/libs/gx2/GX2_Shader.cpp and GX2_shader_legacy.cpp), MPL-2.0. Shader setup and
// uniform functions; packets are identical to Cemu. The legacy gx2Export_* functions (second half of this file) are
// converted to typed GX2:: functions with the same argument order. Exports use CAFE_EXPORT (GX2ShaderInit dropped).
#include <algorithm>

#include "../compat/cemu_export.h"
#include "../latte/isa/LatteConst.h"
#include "../latte/isa/LatteInstructions.h"
#include "../latte/isa/LatteReg.h"
#include "../latte/isa/RegDefines.h"
#include "GX2.h"
#include "GX2_Misc.h"
#include "GX2_Shader.h"

namespace GX2
{
	// Cemu: coreinit::__OSGetProcessSDKVersion(), i.e. <sdk_version> of the title's app.xml. BotW (U-King.rpx v1.5.0)
	// is built with SDK 2.13.01: app.xml sdk_version 21301, RPX file info cafe_sdk_version 0x5335.
	static uint32 _GetProcessSDKVersion()
	{
		return 21301;
	}
}

namespace GX2
{
	using namespace Latte;

	LatteConst::VertexFetchEndianMode _getVtxFormatEndianSwapDefault(uint32 vertexFormat)
	{
		switch (vertexFormat)
		{
		case 0:
		case 1:
		case 4:
		case 10:
			return LatteConst::VertexFetchEndianMode::SWAP_NONE; // 0
		case 2:
		case 3:
		case 7:
		case 8:
		case 14:
		case 15:
			return LatteConst::VertexFetchEndianMode::SWAP_U16; // 1
		case 5:
		case 6:
		case 9:
		case 11:
		case 12:
		case 13:
		case 16:
		case 17:
		case 18:
		case 19:
			return LatteConst::VertexFetchEndianMode::SWAP_U32; // 2
		default:
			break;
		}
		cemu_assert_suspicious();
		return LatteConst::VertexFetchEndianMode::SWAP_NONE;
	}

	uint32 rawFormatToFetchFormat[] =
	{
		1, 2, 5, 6,
		7, 0xD, 0xE, 0xF,
		0x10, 0x16, 0x1A, 0x19,
		0x1D, 0x1E, 0x1F, 0x20,
		0x2F, 0x30, 0x22, 0x23,
	};

	struct GX2AttribDescription
	{
		/* +0x00 */ uint32 location;
		/* +0x04 */ uint32 buffer;
		/* +0x08 */ uint32be offset;
		/* +0x0C */ uint32 format;
		/* +0x10 */ uint32 indexType;
		/* +0x14 */ uint32 aluDivisor;
		/* +0x18 */ uint32 destSel;
		/* +0x1C */ betype<LatteConst::VertexFetchEndianMode> endianSwap;
	};

	static_assert(sizeof(GX2AttribDescription) == 0x20);
	static_assert(sizeof(betype<LatteConst::VertexFetchEndianMode>) == 0x4);

	// calculate size of CF program subpart, includes alignment padding for clause instructions
	size_t _calcFetchShaderCFCodeSize(uint32 attributeCount, GX2FetchShader::FetchShaderType fetchShaderType, uint32 tessellationMode)
	{
		cemu_assert_debug(fetchShaderType == GX2FetchShader::FetchShaderType::NO_TESSELATION);
		cemu_assert_debug(tessellationMode == 0);
		uint32 numCFInstructions = ((attributeCount + 15) / 16) + 1; // one VTX clause can have up to 16 instructions + final CF instruction is RETURN
		size_t cfSize = numCFInstructions * 8;
		cfSize = (cfSize + 0xF) & ~0xF; // pad to 16 byte alignment
		return cfSize;
	}

	size_t _calcFetchShaderClauseCodeSize(uint32 attributeCount, GX2FetchShader::FetchShaderType fetchShaderType, uint32 tessellationMode)
	{
		cemu_assert_debug(fetchShaderType == GX2FetchShader::FetchShaderType::NO_TESSELATION);
		cemu_assert_debug(tessellationMode == 0);
		uint32 numClauseInstructions = attributeCount;
		size_t clauseSize = numClauseInstructions * 16;
		return clauseSize;
	}

	void _writeFetchShaderCFCode(void* programBufferOut, uint32 attributeCount, GX2FetchShader::FetchShaderType fetchShaderType, uint32 tessellationMode)
	{
		LatteCFInstruction* cfInstructionWriter = (LatteCFInstruction*)programBufferOut;
		uint32 attributeIndex = 0;
		uint32 cfSize = (uint32)_calcFetchShaderCFCodeSize(attributeCount, fetchShaderType, tessellationMode);
		while (attributeIndex < attributeCount)
		{
			LatteCFInstruction_DEFAULT defaultInstr;
			defaultInstr.setField_Opcode(LatteCFInstruction::INST_VTX_TC);
			defaultInstr.setField_COUNT(std::min(attributeCount - attributeIndex, 16u));
			defaultInstr.setField_ADDR(cfSize + attributeIndex*16);
			memcpy(cfInstructionWriter, &defaultInstr, sizeof(LatteCFInstruction));
			attributeIndex += 16;
			cfInstructionWriter++;
		}
		// write RETURN instruction
		LatteCFInstruction_DEFAULT returnInstr;
		returnInstr.setField_Opcode(LatteCFInstruction::INST_RETURN);
		returnInstr.setField_BARRIER(true);
		memcpy(cfInstructionWriter, &returnInstr, sizeof(LatteCFInstruction));
	}

	void _writeFetchShaderVTXCode(GX2FetchShader* fetchShader, void* programOut, uint32 attributeCount, GX2AttribDescription* attributeDescription, GX2FetchShader::FetchShaderType fetchShaderType, uint32 tessellationMode)
	{
		uint8* writePtr = (uint8*)programOut;
		// one instruction per attribute (hardcoded into _writeFetchShaderCFCode)
		for (uint32 i = 0; i < attributeCount; i++)
		{
			uint32 attrFormat = _swapEndianU32(attributeDescription[i].format);
			uint32 attrDestSel = _swapEndianU32(attributeDescription[i].destSel);
			uint32 attrLocation = _swapEndianU32(attributeDescription[i].location);
			uint32 attrBufferId = _swapEndianU32(attributeDescription[i].buffer);
			uint32 attrIndexType = _swapEndianU32(attributeDescription[i].indexType);
			uint32 attrAluDivisor = _swapEndianU32(attributeDescription[i].aluDivisor);

			cemu_assert_debug(attrIndexType <= 1);

			LatteConst::VertexFetchEndianMode endianSwap = attributeDescription[i].endianSwap;
			if (endianSwap == LatteConst::VertexFetchEndianMode::SWAP_DEFAULT) // use per-format default
				endianSwap = _getVtxFormatEndianSwapDefault(attrFormat & 0x3F);

			uint32 srcSelX = 0; // this field is used to store the divisor index/mode (0 -> per-vertex index, 1 -> alu divisor 0, 2 -> alu divisor 1, 3 -> per-instance index)
			if (attrIndexType == 0)
			{
				srcSelX = 0; // increase index per vertex
			}
			else if (attrIndexType == 1)
			{
				// instance based index
				if (attrAluDivisor == 1)
				{
					// special encoding if alu divisor is 1
					srcSelX = 3;
				}
				else
				{
					cemu_assert_debug(attrAluDivisor != 0); // divisor should not be zero if instance based index is selected?
					// store alu divisor in divisor table (up to two entries)
					uint32 numDivisors = _swapEndianU32(fetchShader->divisorCount);
					bool divisorFound = false;
					for (uint32 i = 0; i < numDivisors; i++)
					{
						if (fetchShader->divisors[i] == attrAluDivisor)
						{
							srcSelX = i != 0 ? 2 : 1;
							divisorFound = true;
							break;
						}
					}
					if (divisorFound == false)
					{
						// add new divisor
						if (numDivisors >= 2)
						{
							cemu_assert_debug(false); // not enough space for additional divisor
						}
						else
						{
							srcSelX = numDivisors != 0 ? 2 : 1;
							fetchShader->divisors[numDivisors] = attrAluDivisor;
							numDivisors++;
							fetchShader->divisorCount = _swapEndianU32(numDivisors);
						}
					}
				}
			}
			else
			{
				cemu_assert_debug(false);
			}

			// convert attribute format to fetch format
			uint32 fetchFormat = rawFormatToFetchFormat[attrFormat & 0x3F] & 0x3F;

			uint32 nfa = 0;
			if ((attrFormat & 0x800) != 0)
				nfa = 2;
			else if ((attrFormat & 0x100) != 0)
				nfa = 1;
			else
				nfa = 0;

			LatteClauseInstruction_VTX vtxInstruction;
			vtxInstruction.setField_VTX_INST(LatteClauseInstruction_VTX::VTX_INST::_VTX_INST_SEMANTIC);
			vtxInstruction.setFieldSEM_SEMANTIC_ID(attrLocation&0xFF);
			vtxInstruction.setField_BUFFER_ID(attrBufferId + 0xA0);
			vtxInstruction.setField_FETCH_TYPE((LatteConst::VertexFetchType2)attrIndexType);
			vtxInstruction.setField_SRC_SEL_X((LatteClauseInstruction_VTX::SRC_SEL)srcSelX);
			vtxInstruction.setField_DATA_FORMAT((Latte::E_HWFMT)fetchFormat);
			vtxInstruction.setField_NUM_FORMAT_ALL((LatteClauseInstruction_VTX::NUM_FORMAT_ALL)nfa);
			vtxInstruction.setField_OFFSET(attributeDescription[i].offset);
			if ((attrFormat & 0x200) != 0)
				vtxInstruction.setField_FORMAT_COMP_ALL(LatteClauseInstruction_VTX::FORMAT_COMP::COMP_SIGNED);
			vtxInstruction.setField_ENDIAN_SWAP((LatteConst::VertexFetchEndianMode)endianSwap);
			vtxInstruction.setField_DST_SEL(0, (LatteClauseInstruction_VTX::DST_SEL)((attrDestSel >> 24) & 0x7));
			vtxInstruction.setField_DST_SEL(1, (LatteClauseInstruction_VTX::DST_SEL)((attrDestSel >> 16) & 0x7));
			vtxInstruction.setField_DST_SEL(2, (LatteClauseInstruction_VTX::DST_SEL)((attrDestSel >> 8) & 0x7));
			vtxInstruction.setField_DST_SEL(3, (LatteClauseInstruction_VTX::DST_SEL)((attrDestSel >> 0) & 0x7));

			memcpy(writePtr, &vtxInstruction, 16);
			writePtr += 16;
		}
	}

	uint32 GX2CalcFetchShaderSizeEx(uint32 attributeCount, GX2FetchShader::FetchShaderType fetchShaderType, uint32 tessellationMode)
	{
		cemu_assert_debug(fetchShaderType == GX2FetchShader::FetchShaderType::NO_TESSELATION); // other types are todo
		cemu_assert_debug(tessellationMode == 0); // other modes are todo

		uint32 finalSize =
			(uint32)_calcFetchShaderCFCodeSize(attributeCount, fetchShaderType, tessellationMode) +
			(uint32)_calcFetchShaderClauseCodeSize(attributeCount, fetchShaderType, tessellationMode);

		return finalSize;
	}

	void GX2InitFetchShaderEx(GX2FetchShader* fetchShader, void* programBufferOut, uint32 attributeCount, GX2AttribDescription* attributeDescription, GX2FetchShader::FetchShaderType fetchShaderType, uint32 tessellationMode)
	{
		cemu_assert_debug(fetchShaderType == GX2FetchShader::FetchShaderType::NO_TESSELATION);
		cemu_assert_debug(tessellationMode == 0);

		/*
			Fetch shader program:
			[CF_PROGRAM]
			[Last CF instruction: 0x0 0x8A000000 (INST_RETURN)]
			[PAD_TO_16_ALIGNMENT]
			[CLAUSES]
		*/

		memset(fetchShader, 0x00, sizeof(GX2FetchShader));
		fetchShader->attribCount = _swapEndianU32(attributeCount);
		fetchShader->shaderPtr = (MPTR)_swapEndianU32(memory_getVirtualOffsetFromPointer(programBufferOut));

		uint8* shaderStart = (uint8*)programBufferOut;
		uint8* shaderOutput = shaderStart;

		_writeFetchShaderCFCode(shaderOutput, attributeCount, fetchShaderType, tessellationMode);
		shaderOutput += _calcFetchShaderCFCodeSize(attributeCount, fetchShaderType, tessellationMode);
		_writeFetchShaderVTXCode(fetchShader, shaderOutput, attributeCount, attributeDescription, fetchShaderType, tessellationMode);
		shaderOutput += _calcFetchShaderClauseCodeSize(attributeCount, fetchShaderType, tessellationMode);

		uint32 shaderSize = (uint32)(shaderOutput - shaderStart);
		cemu_assert_debug(shaderSize == GX2CalcFetchShaderSizeEx(attributeCount, GX2FetchShader::FetchShaderType::NO_TESSELATION, tessellationMode));

		fetchShader->shaderSize = _swapEndianU32((uint32)(shaderOutput - shaderStart));

		fetchShader->reg_SQ_PGM_RESOURCES_FS = Latte::LATTE_SQ_PGM_RESOURCES_FS().set_NUM_GPRS(2); // todo - affected by tesselation params?
	}

	uint32 GX2GetVertexShaderGPRs(GX2VertexShader* vertexShader)
	{
		return vertexShader->regs.SQ_PGM_RESOURCES_VS.value().get_NUM_GPRS();
	}

	uint32 GX2GetVertexShaderStackEntries(GX2VertexShader* vertexShader)
	{
		return vertexShader->regs.SQ_PGM_RESOURCES_VS.value().get_NUM_STACK_ENTRIES();
	}

	uint32 GX2GetPixelShaderGPRs(GX2PixelShader_t* pixelShader)
	{
		return _swapEndianU32(pixelShader->regs[0])&0xFF;
	}

	uint32 GX2GetPixelShaderStackEntries(GX2PixelShader_t* pixelShader)
	{
		return (_swapEndianU32(pixelShader->regs[0]>>8))&0xFF;
	}

	void GX2SetFetchShader(GX2FetchShader* fetchShaderPtr)
	{
		GX2ReserveCmdSpace(11);
		cemu_assert_debug((_swapEndianU32(fetchShaderPtr->shaderPtr) & 0xFF) == 0);

		gx2WriteGather_submit(
				// setup fetch shader
				pm4HeaderType3(IT_SET_CONTEXT_REG, 1+5),
				Latte::REGADDR::SQ_PGM_START_FS-0xA000,
				_swapEndianU32(fetchShaderPtr->shaderPtr)>>8,
				_swapEndianU32(fetchShaderPtr->shaderSize)>>3,
				0x10000, // ukn (ring buffer size?)
				0x10000, // ukn (ring buffer size?)
				fetchShaderPtr->reg_SQ_PGM_RESOURCES_FS,

				// write instance step
				pm4HeaderType3(IT_SET_CONTEXT_REG, 1+2),
				Latte::REGADDR::VGT_INSTANCE_STEP_RATE_0-0xA000,
				fetchShaderPtr->divisors[0],
				fetchShaderPtr->divisors[1]);
	}

	void GX2SetVertexShader(GX2VertexShader* vertexShader)
	{
		uint32 numOutputIds = vertexShader->regs.vsOutIdTableSize;
		numOutputIds = std::min<uint32>(numOutputIds, 0xA);
		uint32 vsSemanticTableSize = vertexShader->regs.semanticTableSize;

		uint32 reserveSize = 31;
		if (vertexShader->shaderMode == GX2_SHADER_MODE::GEOMETRY_SHADER)
		{
			reserveSize += 7;
		}
		else
		{
			reserveSize += 18;
			reserveSize += numOutputIds;
			if (vertexShader->usesStreamOut != 0)
				reserveSize += 2+12;
		}
		if (vsSemanticTableSize > 0)
		{
			reserveSize += 5 + vsSemanticTableSize;
		}
		GX2ReserveCmdSpace(reserveSize);

		MPTR shaderProgramAddr;
		uint32 shaderProgramSize;
		if (vertexShader->shaderPtr)
		{
			// without R API
			shaderProgramAddr = vertexShader->shaderPtr.GetMPTR();
			shaderProgramSize = vertexShader->shaderSize;
		}
		else
		{
			shaderProgramAddr = vertexShader->rBuffer.GetVirtualAddr();
			shaderProgramSize = vertexShader->rBuffer.GetSize();
		}

		cemu_assert_debug(shaderProgramAddr != 0);
		cemu_assert_debug(shaderProgramSize != 0);

		if (vertexShader->shaderMode == GX2_SHADER_MODE::GEOMETRY_SHADER)
		{
			// in geometry shader mode the vertex shader is written to _ES register and almost all vs control registers are set by GX2SetGeometryShader
			gx2WriteGather_submit(
					pm4HeaderType3(IT_SET_CONTEXT_REG, 6),
					Latte::REGADDR::SQ_PGM_START_ES-0xA000,
					memory_virtualToPhysical(shaderProgramAddr)>>8,
					shaderProgramSize>>3,
					0x100000,
					0x100000,
					vertexShader->regs.SQ_PGM_RESOURCES_VS); // SQ_PGM_RESOURCES_VS/SQ_PGM_RESOURCES_ES
		}
		else
		{
			gx2WriteGather_submit(
					/* vertex shader program */
					pm4HeaderType3(IT_SET_CONTEXT_REG, 6),
					Latte::REGADDR::SQ_PGM_START_VS-0xA000,
					memory_virtualToPhysical(shaderProgramAddr)>>8, // physical address
					shaderProgramSize>>3,
					0x100000,
					0x100000,
					vertexShader->regs.SQ_PGM_RESOURCES_VS, // SQ_PGM_RESOURCES_VS/ES
					/* primitive id enable */
					pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
					Latte::REGADDR::VGT_PRIMITIVEID_EN-0xA000,
					vertexShader->regs.VGT_PRIMITIVEID_EN,
					/* output config */
					pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
					Latte::REGADDR::SPI_VS_OUT_CONFIG-0xA000,
					vertexShader->regs.SPI_VS_OUT_CONFIG,
					/* PA_CL_VS_OUT_CNTL */
					pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
					Latte::REGADDR::PA_CL_VS_OUT_CNTL-0xA000,
					vertexShader->regs.PA_CL_VS_OUT_CNTL
					);

			cemu_assert_debug(vertexShader->regs.SPI_VS_OUT_CONFIG.value().get_VS_PER_COMPONENT() == false); // not handled on the GPU side

			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1+numOutputIds));
			gx2WriteGather_submitU32AsBE(Latte::REGADDR::SPI_VS_OUT_ID_0-0xA000);
			for(uint32 i=0; i<numOutputIds; i++)
				gx2WriteGather_submitU32AsBE(vertexShader->regs.LATTE_SPI_VS_OUT_ID_N[i].value().getRawValue());

			// todo: SQ_PGM_CF_OFFSET_VS
			// todo: VGT_STRMOUT_BUFFER_EN
			// stream out
			if (vertexShader->usesStreamOut != 0)
			{
				// stride 0
				gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
				Latte::REGADDR::VGT_STRMOUT_VTX_STRIDE_0-0xA000,
				vertexShader->streamOutVertexStride[0]>>2,
				// stride 1
				pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
				Latte::REGADDR::VGT_STRMOUT_VTX_STRIDE_1-0xA000,
				vertexShader->streamOutVertexStride[1]>>2,
				// stride 2
				pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
				Latte::REGADDR::VGT_STRMOUT_VTX_STRIDE_2-0xA000,
				vertexShader->streamOutVertexStride[2]>>2,
				// stride 3
				pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
				Latte::REGADDR::VGT_STRMOUT_VTX_STRIDE_3-0xA000,
				vertexShader->streamOutVertexStride[3]>>2);
			}
		}
		// update semantic table
		if (vsSemanticTableSize > 0)
		{
			gx2WriteGather_submit(
					pm4HeaderType3(IT_SET_CONTEXT_REG, 1+1),
					Latte::REGADDR::SQ_VTX_SEMANTIC_CLEAR-0xA000,
					0xFFFFFFFF);
			if (vsSemanticTableSize == 0)
			{
				gx2WriteGather_submit(
						pm4HeaderType3(IT_SET_CONTEXT_REG, 1+1),
						Latte::REGADDR::SQ_VTX_SEMANTIC_0-0xA000,
						0xFFFFFFFF);
			}
			else
			{
				uint32* vsSemanticTable = (uint32*)vertexShader->regs.SQ_VTX_SEMANTIC_N;
				vsSemanticTableSize = std::min<uint32>(vsSemanticTableSize, 32);
				gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1+vsSemanticTableSize));
				gx2WriteGather_submitU32AsBE(Latte::REGADDR::SQ_VTX_SEMANTIC_0-0xA000);
				gx2WriteGather_submitU32AsLEArray(vsSemanticTable, vsSemanticTableSize);
			}
		}
	}

	void _GX2SubmitUniformReg(uint32 offsetRegBase, uint32 aluRegisterOffset, uint32be* dataWords, uint32 sizeInU32s)
	{
		if(aluRegisterOffset&0x8000)
		{
			cemuLog_logDebugOnce(LogType::Force, "_GX2SubmitUniformReg(): Unhandled loop const special case or invalid offset");
			return;
		}
		if((aluRegisterOffset+sizeInU32s) > 0x400)
		{
			cemuLog_logOnce(LogType::APIErrors, "GX2SetVertexUniformReg values are out of range (offset {} + size {} must be equal or smaller than 1024)", aluRegisterOffset, sizeInU32s);
		}
		if( (sizeInU32s&3) != 0)
		{
			cemuLog_logOnce(LogType::APIErrors, "GX2Set*UniformReg must be called with a size that is a multiple of 4 (size: {:})", sizeInU32s);
			sizeInU32s &= ~3;
		}
		GX2ReserveCmdSpace(2 + sizeInU32s);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_ALU_CONST, 1 + sizeInU32s), offsetRegBase + aluRegisterOffset);
		gx2WriteGather_submitU32AsLEArray((uint32*)dataWords, sizeInU32s);
	}

	void GX2SetVertexUniformReg(uint32 offset, uint32 sizeInU32s, uint32be* values)
	{
		_GX2SubmitUniformReg(0x400, offset, values, sizeInU32s);
	}

	void GX2SetPixelUniformReg(uint32 offset, uint32 sizeInU32s, uint32be* values)
	{
		_GX2SubmitUniformReg(0, offset, values, sizeInU32s);
	}

	void GX2SetShaderModeEx(GX2_SHADER_MODE mode, uint32 shaderGprsVS, uint32 shaderStackVS, uint32 shaderGprsGS, uint32 shaderStackGS, uint32 shaderGprsPS, uint32 shaderStackPS)
	{
		if (static_cast<uint32>(mode) > static_cast<uint32>(GX2_SHADER_MODE::COMPUTE_SHADER))
		{
			cemu_assert_suspicious();
			return;
		}
		bool isGeometry = mode == GX2_SHADER_MODE::GEOMETRY_SHADER;
		bool isCompute = mode == GX2_SHADER_MODE::COMPUTE_SHADER;
		bool setThreadGrouping = !isCompute && _GetProcessSDKVersion() >= 21104;
		GX2ReserveCmdSpace((isCompute ? 26 : isGeometry ? 8 : 11) + (setThreadGrouping ? 3 : 0));
		// geometry mode sets this in GX2SetGeometryShader
		if (!isGeometry)
		{
			Latte::LATTE_VGT_GS_MODE gsMode;
			if (isCompute)
			{
				gsMode.set_MODE(Latte::LATTE_VGT_GS_MODE::E_MODE::SCENARIO_G)
					.set_COMPUTE_MODE(Latte::LATTE_VGT_GS_MODE::E_COMPUTE_MODE::ON)
					.set_PARTIAL_THD_AT_EOI(true);
			}
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
				Latte::REGADDR::VGT_GS_MODE - LATTE_REG_BASE_CONTEXT,
				gsMode);
		}

		Latte::LATTE_SQ_CONFIG sqConfig;
		sqConfig.set_DX9_CONSTS(mode == GX2_SHADER_MODE::UNIFORM_REGISTER)
			.set_ALU_INST_PREFER_VECTOR(true)
			.set_PS_PRIO(isCompute ? 0 : 3)
			.set_VS_PRIO(isCompute ? 1 : 2)
			.set_GS_PRIO(isCompute ? 2 : 1)
			.set_ES_PRIO(isCompute ? 3 : 0);
		uint32 threadResource = 0x04043088;
		Latte::LATTE_SQ_GPR_RESOURCE_MGMT_1 gprResource1;
		Latte::LATTE_SQ_GPR_RESOURCE_MGMT_2 gprResource2;
		Latte::LATTE_SQ_STACK_RESOURCE_MGMT_1 stackResource1;
		Latte::LATTE_SQ_STACK_RESOURCE_MGMT_2 stackResource2;
		if (isCompute)
		{
			threadResource = 0xBD010101;
			gprResource2.set_NUM_ES_GPRS(0xF8);
			stackResource2.set_NUM_ES_STACK_ENTRIES(0x100);
		}
		else
		{
			gprResource1.set_NUM_PS_GPRS(shaderGprsPS);
			stackResource1.set_NUM_PS_STACK_ENTRIES(shaderStackPS);
			if (isGeometry)
			{
				// in geometry shader mode, the VS runs in ES and VS runs the geometry copy shader
				gprResource1.set_NUM_VS_GPRS(0x40);
				gprResource2.set_NUM_GS_GPRS(shaderGprsGS).set_NUM_ES_GPRS(shaderGprsVS);
				stackResource2.set_NUM_GS_STACK_ENTRIES(shaderStackGS).set_NUM_ES_STACK_ENTRIES(shaderStackVS);
				threadResource = 0x1C08207C;
			}
			else
			{
				gprResource1.set_NUM_VS_GPRS(shaderGprsVS);
				stackResource1.set_NUM_VS_STACK_ENTRIES(shaderStackVS);
			}
		}

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONFIG_REG, 7),
			Latte::REGADDR::SQ_CONFIG - LATTE_REG_BASE_CONFIG,
			sqConfig, gprResource1.getRawValue() | 0x40000000,
			gprResource2, threadResource, stackResource1, stackResource2);

		if (isCompute)
		{
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONFIG_REG, 5),
				Latte::REGADDR::SQ_ESGS_RING_BASE - LATTE_REG_BASE_CONFIG,
				0, 0xFFFFFF, 0, 0xFFFFFF);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
				Latte::REGADDR::SQ_ESGS_RING_ITEMSIZE - LATTE_REG_BASE_CONTEXT,
				0);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
				Latte::REGADDR::SQ_GSVS_RING_ITEMSIZE - LATTE_REG_BASE_CONTEXT,
				1);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
				Latte::REGADDR::VGT_STRMOUT_EN - LATTE_REG_BASE_CONTEXT,
				0);
		}
		else if (setThreadGrouping)
		{
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_ALL_CONTEXTS, 2),
				Latte::REGADDR::SPI_THREAD_GROUPING - LATTE_REG_BASE_CONTEXT,
				1);
		}
		if (mode != GX2_SHADER_MODE::UNIFORM_REGISTER)
			GX2Invalidate(GX2InvalidationFlag::GPU_SHADER, MPTR_NULL, 0xFFFFFFFF);
	}

}

/****** GX2_shader_legacy.cpp (gx2Export_* converted to typed functions, same argument order) ******/

namespace GX2
{
	// gx2Export_GX2SetPixelShader: r3 = pixelShader
	void GX2SetPixelShader(GX2PixelShader_t* pixelShader)
	{
		uint32 numInputs = _swapEndianU32(pixelShader->regs[4]);
		if( numInputs > 0x20 )
			numInputs = 0x20;

		GX2::GX2ReserveCmdSpace(26 + numInputs);

		MPTR shaderProgramAddr;
		uint32 shaderProgramSize;

		if( _swapEndianU32(pixelShader->shaderPtr) != MPTR_NULL )
		{
			// old format
			shaderProgramAddr = _swapEndianU32(pixelShader->shaderPtr);
			shaderProgramSize = _swapEndianU32(pixelShader->shaderSize);
		}
		else
		{
			shaderProgramAddr = pixelShader->rBuffer.GetVirtualAddr();
			shaderProgramSize = pixelShader->rBuffer.GetSize();
		}

		gx2WriteGather_submit(
			/* pixel shader program */
			pm4HeaderType3(IT_SET_CONTEXT_REG, 6),
			mmSQ_PGM_START_PS - 0xA000,
			memory_virtualToPhysical(shaderProgramAddr)>>8, // address
			shaderProgramSize>>3, // size
			0x100000,
			0x100000,
			_swapEndianU32(pixelShader->regs[0]), // ukn
			/* setup pixel shader input control */
			pm4HeaderType3(IT_SET_CONTEXT_REG, 3),
			mmSPI_PS_IN_CONTROL_0-0xA000,
			_swapEndianU32(pixelShader->regs[2]),
			_swapEndianU32(pixelShader->regs[3]));
		// setup pixel shader extended inputs control
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1+numInputs));
		gx2WriteGather_submitU32AsBE(mmSPI_PS_INPUT_CNTL_0-0xA000);
		for(uint32 i=0; i<numInputs; i++)
		{
			uint32 inputData = _swapEndianU32(pixelShader->regs[5+i]);
			gx2WriteGather_submitU32AsBE(inputData);
		}

		gx2WriteGather_submit(
			/* mmCB_SHADER_MASK */
			pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
			mmCB_SHADER_MASK-0xA000,
			_swapEndianU32(pixelShader->regs[37]),
			/* mmCB_SHADER_CONTROL */
			pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
			mmCB_SHADER_CONTROL-0xA000,
			_swapEndianU32(pixelShader->regs[38]),
			/* mmDB_SHADER_CONTROL */
			pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
			mmDB_SHADER_CONTROL-0xA000,
			_swapEndianU32(pixelShader->regs[39]),
			/* SPI_INPUT_Z */
			pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
			mmSPI_INPUT_Z-0xA000,
			_swapEndianU32(pixelShader->regs[40]));
	}

	// gx2Export_GX2SetGeometryShader: r3 = geometryShader
	void GX2SetGeometryShader(GX2GeometryShader_t* geometryShader)
	{
		uint32 numOutputIds = _swapEndianU32(geometryShader->regs[7]);
		numOutputIds = std::min<uint32>(numOutputIds, 0xA);
		uint32 reserveSize = 38; // 38 fixed parameters
		if (numOutputIds != 0)
			reserveSize += 2 + numOutputIds;
		if( _swapEndianU32(geometryShader->useStreamout) != 0 )
			reserveSize += 2 + 12;

		GX2::GX2ReserveCmdSpace(reserveSize);

		MPTR shaderProgramAddr;
		uint32 shaderProgramSize;

		if( _swapEndianU32(geometryShader->shaderPtr) != MPTR_NULL )
		{
			// old format
			shaderProgramAddr = _swapEndianU32(geometryShader->shaderPtr);
			shaderProgramSize = _swapEndianU32(geometryShader->shaderSize);
		}
		else
		{
			shaderProgramAddr = geometryShader->rBuffer.GetVirtualAddr();
			shaderProgramSize = geometryShader->rBuffer.GetSize();
		}

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 6));
		gx2WriteGather_submitU32AsBE(mmSQ_PGM_START_GS-0xA000);
		gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(shaderProgramAddr)>>8);
		gx2WriteGather_submitU32AsBE(shaderProgramSize>>3);
		gx2WriteGather_submitU32AsBE(0x100000);
		gx2WriteGather_submitU32AsBE(0x100000);
		gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->regs[0])); // unknown content (SQ_PGM_RESOURCES_GS)

		uint32 primitiveOut = _swapEndianU32(geometryShader->regs[1]);
		(void)primitiveOut;

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
		gx2WriteGather_submitU32AsBE(mmVGT_GS_OUT_PRIM_TYPE-0xA000);
		gx2WriteGather_submitU32AsLE(geometryShader->regs[1]);

		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 2),
			Latte::REGADDR::VGT_GS_MODE - 0xA000,
			geometryShader->reg.VGT_GS_MODE
		);

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
		gx2WriteGather_submitU32AsBE(mmSQ_PGM_RESOURCES_GS-0xA000);
		gx2WriteGather_submitU32AsLE(geometryShader->regs[0]);

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
		gx2WriteGather_submitU32AsBE(mmSQ_GS_VERT_ITEMSIZE-0xA000);
		gx2WriteGather_submitU32AsLE(geometryShader->regs[5]);

		if( _swapEndianU32(geometryShader->useStreamout) != 0 )
		{
			// todo - IT_EVENT_WRITE packet here
			// stride 0
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
			gx2WriteGather_submitU32AsBE(mmVGT_STRMOUT_VTX_STRIDE_0-0xA000);
			gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->streamoutStride[0])>>2);
			// stride 1
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
			gx2WriteGather_submitU32AsBE(mmVGT_STRMOUT_VTX_STRIDE_1-0xA000);
			gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->streamoutStride[1])>>2);
			// stride 2
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
			gx2WriteGather_submitU32AsBE(mmVGT_STRMOUT_VTX_STRIDE_2-0xA000);
			gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->streamoutStride[2])>>2);
			// stride 3
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
			gx2WriteGather_submitU32AsBE(mmVGT_STRMOUT_VTX_STRIDE_3-0xA000);
			gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->streamoutStride[3])>>2);
		}

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
		gx2WriteGather_submitU32AsBE(mmVGT_STRMOUT_BUFFER_EN-0xA000);
		gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->regs[18]));

		// set copy shader (written to vertex shader registers, vs in turn is written to es registers)
		MPTR copyShaderProgramAddr;
		uint32 copyShaderProgramSize;
		if( _swapEndianU32(geometryShader->copyShaderPtr) != MPTR_NULL )
		{
			copyShaderProgramAddr = _swapEndianU32(geometryShader->copyShaderPtr);
			copyShaderProgramSize = _swapEndianU32(geometryShader->copyShaderSize);
		}
		else
		{
			copyShaderProgramAddr = geometryShader->rBufferCopyProgram.GetVirtualAddr();
			copyShaderProgramSize = geometryShader->rBufferCopyProgram.GetSize();
		}

		cemu_assert_debug((copyShaderProgramAddr>>8) != 0);
		cemu_assert_debug((copyShaderProgramSize>>3) != 0);

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 6));
		gx2WriteGather_submitU32AsBE(mmSQ_PGM_START_VS-0xA000);
		gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(copyShaderProgramAddr)>>8);
		gx2WriteGather_submitU32AsBE(copyShaderProgramSize>>3);
		gx2WriteGather_submitU32AsBE(0x100000);
		gx2WriteGather_submitU32AsBE(0x100000);
		gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->regs[4])); // mmSQ_PGM_RESOURCES_VS

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
		gx2WriteGather_submitU32AsBE(mmPA_CL_VS_OUT_CNTL-0xA000);
		gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->regs[3]));

		// GS outputs
		if( numOutputIds != 0 )
		{
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1+numOutputIds));
			gx2WriteGather_submitU32AsBE(mmSPI_VS_OUT_ID_0-0xA000);
			for(uint32 i=0; i<numOutputIds; i++)
			{
				gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->regs[8+i]));
			}
		}

		// output config
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
		gx2WriteGather_submitU32AsBE(mmSPI_VS_OUT_CONFIG-0xA000);
		gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->regs[6]));

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 2));
		gx2WriteGather_submitU32AsBE(mmSQ_GSVS_RING_ITEMSIZE-0xA000);
		gx2WriteGather_submitU32AsBE(_swapEndianU32(geometryShader->ringItemsize)&0x7FFF);

		/*
		  Geometry shader registers in regs[19]:
		  0		SQ_PGM_RESOURCES_GS ?
		  1		mmVGT_GS_OUT_PRIM_TYPE
		  2		mmVGT_GS_MODE
		  3		mmPA_CL_VS_OUT_CNTL
		  4		mmSQ_PGM_RESOURCES_VS (set in combination with mmSQ_PGM_START_VS)
		  5		mmSQ_GS_VERT_ITEMSIZE
		  6		mmSPI_VS_OUT_CONFIG
		  7		number of active mmSPI_VS_OUT_ID_* fields?
		  8-17  mmSPI_VS_OUT_ID_*
		  18	mmVGT_STRMOUT_BUFFER_EN
		 */
	}

	// gx2Export_GX2SetComputeShader: r3 = computeShader
	void GX2SetComputeShader(GX2ComputeShader* computeShader)
	{
		MPTR shaderPtr;
		uint32 shaderSize;
		if (computeShader->programPtr)
		{
			shaderPtr = computeShader->programPtr;
			shaderSize = computeShader->programSize;
		}
		else
		{
			shaderPtr = computeShader->rBuffer.GetVirtualAddr();
			shaderSize = computeShader->rBuffer.GetSize();
		}
		GX2::GX2ReserveCmdSpace(0x11);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 6),
			mmSQ_PGM_START_ES-0xA000,
			memory_virtualToPhysical(shaderPtr) >> 8,
			shaderSize >> 3,
			0x100000,
			0x100000,
			computeShader->regs[0]);

		// todo: Other registers
	}

	void _GX2SubmitUniformBlock(uint32 registerBase, uint32 index, MPTR virtualAddress, uint32 size)
	{
		GX2::GX2ReserveCmdSpace(9);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_RESOURCE, 8),
			registerBase + index * 7,
			memory_virtualToPhysical(virtualAddress),
			size - 1,
			0,
			1,
			0, // ukn
			0, // ukn
			0xC0000000);
	}

	// gx2Export_GX2SetVertexUniformBlock: r3 = index, r4 = size, r5 = address
	void GX2SetVertexUniformBlock(uint32 index, uint32 size, MPTR data)
	{
		_GX2SubmitUniformBlock(mmSQ_VTX_UNIFORM_BLOCK_START - mmSQ_TEX_RESOURCE_WORD0, index, data, size);
	}

	// gx2Export_GX2SetPixelUniformBlock: r3 = index, r4 = size, r5 = address
	void GX2SetPixelUniformBlock(uint32 index, uint32 size, MPTR data)
	{
		_GX2SubmitUniformBlock(mmSQ_PS_UNIFORM_BLOCK_START - mmSQ_TEX_RESOURCE_WORD0, index, data, size);
	}

	// gx2Export_GX2SetGeometryUniformBlock: r3 = index, r4 = size, r5 = address
	void GX2SetGeometryUniformBlock(uint32 index, uint32 size, MPTR data)
	{
		_GX2SubmitUniformBlock(mmSQ_GS_UNIFORM_BLOCK_START - mmSQ_TEX_RESOURCE_WORD0, index, data, size);
	}

	// gx2Export_GX2RSetVertexUniformBlock: r3 = buffer, r4 = index, r5 = offset
	void GX2RSetVertexUniformBlock(GX2RBuffer* bufferPtr, uint32 index, uint32 offset)
	{
		GX2::GX2ReserveCmdSpace(9);
		_GX2SubmitUniformBlock(mmSQ_VTX_UNIFORM_BLOCK_START - mmSQ_TEX_RESOURCE_WORD0, index, bufferPtr->GetVirtualAddr() + offset, bufferPtr->GetSize() - offset);
	}

	// gx2Export_GX2CalcGeometryShaderInputRingBufferSize: r3 = ringItemSize
	uint32 GX2CalcGeometryShaderInputRingBufferSize(uint32 ringItemSize)
	{
		uint32 size = (ringItemSize*4) * 0x1000;
		return size;
	}

	// gx2Export_GX2CalcGeometryShaderOutputRingBufferSize: r3 = ringItemSize
	uint32 GX2CalcGeometryShaderOutputRingBufferSize(uint32 ringItemSize)
	{
		uint32 size = (ringItemSize*4) * 0x1000;
		return size;
	}
}

CAFE_EXPORT(gx2, GX2CalcFetchShaderSizeEx, GX2::GX2CalcFetchShaderSizeEx);
CAFE_EXPORT(gx2, GX2InitFetchShaderEx, GX2::GX2InitFetchShaderEx);
CAFE_EXPORT(gx2, GX2GetVertexShaderGPRs, GX2::GX2GetVertexShaderGPRs);
CAFE_EXPORT(gx2, GX2GetVertexShaderStackEntries, GX2::GX2GetVertexShaderStackEntries);
CAFE_EXPORT(gx2, GX2GetPixelShaderGPRs, GX2::GX2GetPixelShaderGPRs);
CAFE_EXPORT(gx2, GX2GetPixelShaderStackEntries, GX2::GX2GetPixelShaderStackEntries);
CAFE_EXPORT(gx2, GX2SetFetchShader, GX2::GX2SetFetchShader);
CAFE_EXPORT(gx2, GX2SetVertexShader, GX2::GX2SetVertexShader);
CAFE_EXPORT(gx2, GX2SetVertexUniformReg, GX2::GX2SetVertexUniformReg);
CAFE_EXPORT(gx2, GX2SetPixelUniformReg, GX2::GX2SetPixelUniformReg);
CAFE_EXPORT(gx2, GX2SetShaderModeEx, GX2::GX2SetShaderModeEx);
// GX2_shader_legacy.cpp exports (registered with osLib_addFunction in Cemu's GX2.cpp)
CAFE_EXPORT(gx2, GX2SetPixelShader, GX2::GX2SetPixelShader);
CAFE_EXPORT(gx2, GX2SetGeometryShader, GX2::GX2SetGeometryShader);
CAFE_EXPORT(gx2, GX2SetComputeShader, GX2::GX2SetComputeShader);
CAFE_EXPORT(gx2, GX2SetVertexUniformBlock, GX2::GX2SetVertexUniformBlock);
CAFE_EXPORT(gx2, GX2RSetVertexUniformBlock, GX2::GX2RSetVertexUniformBlock);
CAFE_EXPORT(gx2, GX2SetPixelUniformBlock, GX2::GX2SetPixelUniformBlock);
CAFE_EXPORT(gx2, GX2SetGeometryUniformBlock, GX2::GX2SetGeometryUniformBlock);
CAFE_EXPORT(gx2, GX2CalcGeometryShaderInputRingBufferSize, GX2::GX2CalcGeometryShaderInputRingBufferSize);
CAFE_EXPORT(gx2, GX2CalcGeometryShaderOutputRingBufferSize, GX2::GX2CalcGeometryShaderOutputRingBufferSize);
