
#include "Globals.h"

#include "BlockEntities/ChestEntity.h"

typedef cItemCallback<cChestEntity> cChestCallback;

#include "Chunk.h"
#include "World.h"
#include "Blocks/GetHandlerCompileTimeTemplate.h"
#include "Blocks/BlockTorch.h"
#include "Blocks/BlockLever.h"
#include "Blocks/BlockButton.h"
#include "Blocks/BlockTripwireHook.h"
#include "Blocks/BlockDoor.h"
#include "Blocks/BlockPiston.h"


#include "IncrementalRedstoneSimulator.h"
#include "BoundingBox.h"
#include "Blocks/ChunkInterface.h"
#include "RedstoneSimulator.h"





void cIncrementalRedstoneSimulator::RedstoneAddBlock(int a_BlockX, int a_BlockY, int a_BlockZ, cChunk * a_Chunk, cChunk * a_OtherChunk)
{
	if ((a_Chunk == nullptr) || !a_Chunk->IsValid())
	{
		return;
	}
	else if ((a_BlockY < 0) || (a_BlockY > cChunkDef::Height))
	{
		return;
	}

	// We may be called with coordinates in a chunk that is not the first chunk parameter
	// In that case, the actual chunk (which the coordinates are in), will be passed as the second parameter
	// Use that Chunk pointer to get a relative position

	int RelX = 0;
	int RelZ = 0;
	BLOCKTYPE Block;
	NIBBLETYPE Meta;

	if (a_OtherChunk != nullptr)
	{
		RelX = a_BlockX - a_OtherChunk->GetPosX() * cChunkDef::Width;
		RelZ = a_BlockZ - a_OtherChunk->GetPosZ() * cChunkDef::Width;
		a_OtherChunk->GetBlockTypeMeta(RelX, a_BlockY, RelZ, Block, Meta);

		// If a_OtherChunk is passed (not nullptr), it is the chunk that had a block change, and a_Chunk will be the neighbouring chunk of that block
		// Because said neighbouring chunk does not know of this change but still needs to update its redstone, we set it to dirty
		a_Chunk->SetIsRedstoneDirty(true);
	}
	else
	{
		RelX = a_BlockX - a_Chunk->GetPosX() * cChunkDef::Width;
		RelZ = a_BlockZ - a_Chunk->GetPosZ() * cChunkDef::Width;
		a_Chunk->GetBlockTypeMeta(RelX, a_BlockY, RelZ, Block, Meta);
	}

	// Every time a block is changed (AddBlock called), we want to go through all lists and check to see if the coordiantes stored within are still valid
	// Checking only when a block is changed, as opposed to every tick, also improves performance

	if (
		!IsPotentialSource(Block) ||
		(
			// Changeable sources
			((Block == E_BLOCK_REDSTONE_WIRE) && (Meta == 0)) ||
			((Block == E_BLOCK_LEVER) && !IsLeverOn(Meta)) ||
			((Block == E_BLOCK_DETECTOR_RAIL) && ((Meta & 0x08) == 0)) ||
			(((Block == E_BLOCK_STONE_BUTTON) || (Block == E_BLOCK_WOODEN_BUTTON)) && (!IsButtonOn(Meta))) ||
			((Block == E_BLOCK_TRIPWIRE_HOOK) && ((Meta & 0x08) == 0))
		)
	)
	{
		SetSourceUnpowered(RelX, a_BlockY, RelZ, a_OtherChunk != nullptr ? a_OtherChunk : a_Chunk);
	}

	if (!IsViableMiddleBlock(Block))
	{
		SetInvalidMiddleBlock(RelX, a_BlockY, RelZ, a_OtherChunk != nullptr ? a_OtherChunk : a_Chunk);
	}

	auto & SimulatedPlayerToggleableBlocks = ((cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData())->m_SimulatedPlayerToggleableBlocks;
	SimulatedPlayerToggleableBlocks.erase(std::remove_if(SimulatedPlayerToggleableBlocks.begin(), SimulatedPlayerToggleableBlocks.end(), [RelX, a_BlockY, RelZ, Block, this](const sSimulatedPlayerToggleableList & itr)
		{
			return itr.a_RelBlockPos.Equals(Vector3i(RelX, a_BlockY, RelZ)) && !IsAllowedBlock(Block);
		}
	), SimulatedPlayerToggleableBlocks.end());

	
	auto & RepeatersDelayList = ((cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData())->m_RepeatersDelayList;
	RepeatersDelayList.erase(std::remove_if(RepeatersDelayList.begin(), RepeatersDelayList.end(), [RelX, a_BlockY, RelZ, Block](const sRepeatersDelayList & itr)
		{
			return itr.a_RelBlockPos.Equals(Vector3i(RelX, a_BlockY, RelZ)) && (Block != E_BLOCK_REDSTONE_REPEATER_ON) && (Block != E_BLOCK_REDSTONE_REPEATER_OFF);
		}
	), RepeatersDelayList.end());

	if (a_OtherChunk != nullptr)
	{
		// DO NOT touch our chunk's data structure if we are being called with coordinates from another chunk - this one caused me massive grief :P
		return;
	}

	cCoordWithBlockAndBoolVector & RedstoneSimulatorChunkData = ((cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData())->m_ChunkData;
	for (auto & itr : RedstoneSimulatorChunkData)
	{
		if ((itr.x == RelX) && (itr.y == a_BlockY) && (itr.z == RelZ))  // We are at an entry matching the current (changed) block
		{
			if (!IsAllowedBlock(Block))
			{
				itr.DataTwo = true;  // The new blocktype is not redstone; it must be queued to be removed from this list
			}
			else
			{
				itr.DataTwo = false;
				itr.Data = Block;  // Update block information
			}
			return;
		}
	}

	if (!IsAllowedBlock(Block))
	{
		return;
	}

	cCoordWithBlockAndBoolVector & QueuedData = ((cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData())->m_QueuedChunkData;
	for (const auto & itr : QueuedData)
	{
		if ((itr.x == RelX) && (itr.y == a_BlockY) && (itr.z == RelZ))
		{
			// Can't have duplicates in here either, in case something adds the block again before the structure can written to the main chunk data
			return;
		}
	}
	QueuedData.emplace_back(cCoordWithBlockAndBool(RelX, a_BlockY, RelZ, Block, false));
}





void cIncrementalRedstoneSimulator::SimulateChunk(float a_Dt, int a_ChunkX, int a_ChunkZ, cChunk * a_Chunk)
{
	m_RedstoneSimulatorChunkData = (cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData();
	if (m_RedstoneSimulatorChunkData == nullptr)
	{
		m_RedstoneSimulatorChunkData = new cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData();
		a_Chunk->SetRedstoneSimulatorData(m_RedstoneSimulatorChunkData);
	}
	if (m_RedstoneSimulatorChunkData->m_ChunkData.empty() && ((cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData())->m_QueuedChunkData.empty())
	{
		return;
	}

	m_RedstoneSimulatorChunkData->m_ChunkData.insert(
		m_RedstoneSimulatorChunkData->m_ChunkData.end(),
		m_RedstoneSimulatorChunkData->m_QueuedChunkData.begin(),
		m_RedstoneSimulatorChunkData->m_QueuedChunkData.end()
	);

	m_RedstoneSimulatorChunkData->m_QueuedChunkData.clear();

	m_PoweredBlocks = &m_RedstoneSimulatorChunkData->m_PoweredBlocks;
	m_RepeatersDelayList = &m_RedstoneSimulatorChunkData->m_RepeatersDelayList;
	m_SimulatedPlayerToggleableBlocks = &m_RedstoneSimulatorChunkData->m_SimulatedPlayerToggleableBlocks;
	m_LinkedPoweredBlocks = &m_RedstoneSimulatorChunkData->m_LinkedBlocks;
	m_Chunk = a_Chunk;
	bool ShouldUpdateSimulateOnceBlocks = false;

	if (a_Chunk->IsRedstoneDirty())
	{
		// Simulate the majority of devices only if something (blockwise or power-wise) has changed
		// Make sure to allow the chunk to resimulate after the initial run if there was a power change (ShouldUpdateSimulateOnceBlocks helps to do this)
		a_Chunk->SetIsRedstoneDirty(false);
		ShouldUpdateSimulateOnceBlocks = true;
	}

	HandleRedstoneRepeaterDelays();

	for (auto dataitr = m_RedstoneSimulatorChunkData->m_ChunkData.begin(); dataitr != m_RedstoneSimulatorChunkData->m_ChunkData.end();)
	{
		if (dataitr->DataTwo)
		{
			dataitr = m_RedstoneSimulatorChunkData->m_ChunkData.erase(dataitr);
			continue;
		}

		switch (dataitr->Data)
		{
			case E_BLOCK_DAYLIGHT_SENSOR: HandleDaylightSensor(dataitr->x, dataitr->y, dataitr->z); break;
			case E_BLOCK_TRIPWIRE:        HandleTripwire(dataitr->x, dataitr->y, dataitr->z);       break;
			case E_BLOCK_TRIPWIRE_HOOK:   HandleTripwireHook(dataitr->x, dataitr->y, dataitr->z);   break;

			case E_BLOCK_WOODEN_PRESSURE_PLATE:
			case E_BLOCK_STONE_PRESSURE_PLATE:
			case E_BLOCK_LIGHT_WEIGHTED_PRESSURE_PLATE:
			case E_BLOCK_HEAVY_WEIGHTED_PRESSURE_PLATE:
			{
				HandlePressurePlate(dataitr->x, dataitr->y, dataitr->z, dataitr->Data);
				break;
			}
			default: break;
		}

		if (ShouldUpdateSimulateOnceBlocks)
		{
			switch (dataitr->Data)
			{
				case E_BLOCK_REDSTONE_WIRE:         HandleRedstoneWire(dataitr->x, dataitr->y, dataitr->z);	  break;
				case E_BLOCK_COMMAND_BLOCK:         HandleCommandBlock(dataitr->x, dataitr->y, dataitr->z);   break;
				case E_BLOCK_NOTE_BLOCK:            HandleNoteBlock(dataitr->x, dataitr->y, dataitr->z);      break;
				case E_BLOCK_BLOCK_OF_REDSTONE:     HandleRedstoneBlock(dataitr->x, dataitr->y, dataitr->z);  break;
				case E_BLOCK_LEVER:                 HandleRedstoneLever(dataitr->x, dataitr->y, dataitr->z);  break;
				case E_BLOCK_TNT:                   HandleTNT(dataitr->x, dataitr->y, dataitr->z);            break;
				case E_BLOCK_IRON_TRAPDOOR:         HandleTrapdoor(dataitr->x, dataitr->y, dataitr->z);       break;
				case E_BLOCK_TRAPDOOR:              HandleTrapdoor(dataitr->x, dataitr->y, dataitr->z);       break;
				case E_BLOCK_TRAPPED_CHEST:         HandleTrappedChest(dataitr->x, dataitr->y, dataitr->z);   break;

				case E_BLOCK_ACTIVATOR_RAIL:
				case E_BLOCK_DETECTOR_RAIL:
				case E_BLOCK_POWERED_RAIL:
				{
					HandleRail(dataitr->x, dataitr->y, dataitr->z, dataitr->Data);
					break;
				}
				case E_BLOCK_ACACIA_DOOR:
				case E_BLOCK_BIRCH_DOOR:
				case E_BLOCK_DARK_OAK_DOOR:
				case E_BLOCK_JUNGLE_DOOR:
				case E_BLOCK_SPRUCE_DOOR:
				case E_BLOCK_WOODEN_DOOR:
				case E_BLOCK_IRON_DOOR:
				{
					HandleDoor(dataitr->x, dataitr->y, dataitr->z);
					break;
				}
				case E_BLOCK_ACACIA_FENCE_GATE:
				case E_BLOCK_BIRCH_FENCE_GATE:
				case E_BLOCK_DARK_OAK_FENCE_GATE:
				case E_BLOCK_FENCE_GATE:
				case E_BLOCK_JUNGLE_FENCE_GATE:
				case E_BLOCK_SPRUCE_FENCE_GATE:
				{
					HandleFenceGate(dataitr->x, dataitr->y, dataitr->z);
					break;
				}
				case E_BLOCK_REDSTONE_LAMP_OFF:
				case E_BLOCK_REDSTONE_LAMP_ON:
				{
					HandleRedstoneLamp(dataitr->x, dataitr->y, dataitr->z, dataitr->Data);
					break;
				}
				case E_BLOCK_DISPENSER:
				case E_BLOCK_DROPPER:
				{
					HandleDropSpenser(dataitr->x, dataitr->y, dataitr->z);
					break;
				}
				case E_BLOCK_PISTON:
				case E_BLOCK_STICKY_PISTON:
				{
					HandlePiston(dataitr->x, dataitr->y, dataitr->z);
					break;
				}
				case E_BLOCK_REDSTONE_REPEATER_OFF:
				case E_BLOCK_REDSTONE_REPEATER_ON:
				{
					HandleRedstoneRepeater(dataitr->x, dataitr->y, dataitr->z, dataitr->Data);
					break;
				}
				case E_BLOCK_REDSTONE_TORCH_OFF:
				case E_BLOCK_REDSTONE_TORCH_ON:
				{
					HandleRedstoneTorch(dataitr->x, dataitr->y, dataitr->z, dataitr->Data);
					break;
				}
				case E_BLOCK_STONE_BUTTON:
				case E_BLOCK_WOODEN_BUTTON:
				{
					HandleRedstoneButton(dataitr->x, dataitr->y, dataitr->z);
					break;
				}
				default: break;
			}
		}
		++dataitr;
	}
}





void cIncrementalRedstoneSimulator::WakeUp(int a_BlockX, int a_BlockY, int a_BlockZ, cChunk * a_Chunk)
{
	if (AreCoordsOnChunkBoundary(a_BlockX, a_BlockY, a_BlockZ))
	{
		// On a chunk boundary, alert all four sides (i.e. at least one neighbouring chunk)
		AddBlock(a_BlockX, a_BlockY, a_BlockZ, a_Chunk);

		// Pass the original coordinates, because when adding things to our simulator lists, we get the chunk that they are in, and therefore any updates need to preseve their position
		// RedstoneAddBlock to pass both the neighbouring chunk and the chunk which the coordinates are in and +- 2 in GetNeighbour() to accomodate for LinkedPowered blocks being 2 away from chunk boundaries
		RedstoneAddBlock(a_BlockX, a_BlockY, a_BlockZ, a_Chunk->GetNeighborChunk(a_BlockX - 2, a_BlockZ), a_Chunk);
		RedstoneAddBlock(a_BlockX, a_BlockY, a_BlockZ, a_Chunk->GetNeighborChunk(a_BlockX + 2, a_BlockZ), a_Chunk);
		RedstoneAddBlock(a_BlockX, a_BlockY, a_BlockZ, a_Chunk->GetNeighborChunk(a_BlockX, a_BlockZ - 2), a_Chunk);
		RedstoneAddBlock(a_BlockX, a_BlockY, a_BlockZ, a_Chunk->GetNeighborChunk(a_BlockX, a_BlockZ + 2), a_Chunk);

		return;
	}

	// Not on boundary, just alert this chunk for speed
	AddBlock(a_BlockX, a_BlockY, a_BlockZ, a_Chunk);
}





void cIncrementalRedstoneSimulator::HandleRedstoneTorch(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, BLOCKTYPE a_MyState)
{
	static const struct  // Define which directions the torch can power
	{
		int x, y, z;
	} gCrossCoords[] =
	{
		{ 1, 0, 0 },
		{ -1, 0, 0 },
		{ 0, 0, 1 },
		{ 0, 0, -1 },
		{ 0, 1, 0 },
	};

	if (a_MyState == E_BLOCK_REDSTONE_TORCH_ON)
	{
		// Check if the block the torch is on is powered
		int X = a_RelBlockX; int Y = a_RelBlockY; int Z = a_RelBlockZ;
		AddFaceDirection(X, Y, Z, GetHandlerCompileTime<E_BLOCK_TORCH>::type::MetaDataToDirection(m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ)), true);  // Inverse true to get the block torch is on

		cChunk * Neighbour = m_Chunk->GetRelNeighborChunkAdjustCoords(X, Z);
		if ((Neighbour == nullptr) || !Neighbour->IsValid())
		{
			return;
		}

		if (AreCoordsDirectlyPowered(X, Y, Z, Neighbour))
		{
			// There was a match, torch goes off
			m_Chunk->SetBlock(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_BLOCK_REDSTONE_TORCH_OFF, m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ));
			return;
		}

		// Torch still on, make all 4(X, Z) + 1(Y) sides powered
		for (size_t i = 0; i < ARRAYCOUNT(gCrossCoords); i++)
		{
			BLOCKTYPE Type = 0;
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX + gCrossCoords[i].x, a_RelBlockY + gCrossCoords[i].y, a_RelBlockZ + gCrossCoords[i].z, Type))
			{
				continue;
			}
			if (i + 1 < ARRAYCOUNT(gCrossCoords))  // Sides of torch, not top (top is last)
			{
				if (
					IsMechanism(Type) &&  // Is it a mechanism? Not block/other torch etc.
					(!Vector3i(a_RelBlockX + gCrossCoords[i].x, a_RelBlockY + gCrossCoords[i].y, a_RelBlockZ + gCrossCoords[i].z).Equals(Vector3i(X, Y, Z)))  // CAN'T power block is that it is on
					)
				{
					SetBlockPowered(a_RelBlockX + gCrossCoords[i].x, a_RelBlockY + gCrossCoords[i].y, a_RelBlockZ + gCrossCoords[i].z, a_RelBlockX, a_RelBlockY, a_RelBlockZ);
				}
			}
			else
			{
				// Top side, power whatever is there, including blocks
				SetBlockPowered(a_RelBlockX + gCrossCoords[i].x, a_RelBlockY + gCrossCoords[i].y, a_RelBlockZ + gCrossCoords[i].z, a_RelBlockX, a_RelBlockY, a_RelBlockZ);
				// Power all blocks surrounding block above torch
				SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_YP);
			}
		}

		if (m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) != 0x5)  // Is torch standing on ground? If NOT (i.e. on wall), power block beneath
		{
			BLOCKTYPE Type = m_Chunk->GetBlock(a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ);

			if (IsMechanism(Type))  // Still can't make a normal block powered though!
			{
				SetBlockPowered(a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ);
			}
		}
	}
	else
	{
		// Check if the block the torch is on is powered
		int X = a_RelBlockX; int Y = a_RelBlockY; int Z = a_RelBlockZ;
		AddFaceDirection(X, Y, Z, GetHandlerCompileTime<E_BLOCK_TORCH>::type::MetaDataToDirection(m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ)), true);  // Inverse true to get the block torch is on

		cChunk * Neighbour = m_Chunk->GetRelNeighborChunkAdjustCoords(X, Z);
		if ((Neighbour == nullptr) || !Neighbour->IsValid())
		{
			return;
		}

		// See if off state torch can be turned on again
		if (AreCoordsDirectlyPowered(X, Y, Z, Neighbour))
		{
			return;  // Something matches, torch still powered
		}

		// Block torch on not powered, can be turned on again!
		m_Chunk->SetBlock(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_BLOCK_REDSTONE_TORCH_ON, m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ));
	}
}





void cIncrementalRedstoneSimulator::HandleRedstoneBlock(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
	SetBlockPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ);  // Set self as powered
}





void cIncrementalRedstoneSimulator::HandleRedstoneLever(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	NIBBLETYPE Meta = m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
	if (IsLeverOn(Meta))
	{
		SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ);

		eBlockFace Dir = GetHandlerCompileTime<E_BLOCK_LEVER>::type::BlockMetaDataToBlockFace(Meta);

		Dir = ReverseBlockFace(Dir);

		SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, Dir);
	}
}





void cIncrementalRedstoneSimulator::HandleFenceGate(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
	int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;
	NIBBLETYPE MetaData = m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ);

	if (AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ))
	{
		if (!AreCoordsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, true))
		{
			if ((MetaData & 0x4) == 0)
			{
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, MetaData | 0x4);
				m_Chunk->BroadcastSoundParticleEffect(1003, BlockX, a_RelBlockY, BlockZ, 0);
			}
			SetPlayerToggleableBlockAsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, true);
		}
	}
	else
	{
		if (!AreCoordsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, false))
		{
			if ((MetaData & 0x4) != 0)
			{
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, MetaData & ~0x04);
				m_Chunk->BroadcastSoundParticleEffect(1003, BlockX, a_RelBlockY, BlockZ, 0);
			}
			SetPlayerToggleableBlockAsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, false);
		}
	}
}





void cIncrementalRedstoneSimulator::HandleRedstoneButton(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	NIBBLETYPE Meta = m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
	if (IsButtonOn(Meta))
	{
		SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ);

		eBlockFace Dir = GetHandlerCompileTime<E_BLOCK_STONE_BUTTON>::type::BlockMetaDataToBlockFace(Meta);
		Dir = ReverseBlockFace(Dir);
		SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, Dir);
	}
}





void cIncrementalRedstoneSimulator::HandleRedstoneWire(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	static const struct  // Define which directions the wire can receive power from
	{
		int x, y, z;
	} gCrossCoords[] =
	{
		{ 1, 0, 0 }, /* Wires on same level start */
		{ -1, 0, 0 },
		{ 0, 0, 1 },
		{ 0, 0, -1 }, /* Wires on same level stop */
		{ 1, 1, 0 }, /* Wires one higher, surrounding self start */
		{ -1, 1, 0 },
		{ 0, 1, 1 },
		{ 0, 1, -1 }, /* Wires one higher, surrounding self stop */
		{ 1, -1, 0 }, /* Wires one lower, surrounding self start */
		{ -1, -1, 0 },
		{ 0, -1, 1 },
		{ 0, -1, -1 }, /* Wires one lower, surrounding self stop */
	};

	static const struct  // Define which directions the wire will check for repeater prescence
	{
		int x, y, z;
	} gSideCoords[] =
	{
		{ 1, 0, 0 },
		{ -1, 0, 0 },
		{ 0, 0, 1 },
		{ 0, 0, -1 },
		{ 0, 1, 0 },
	};

	// Check to see if directly beside a power source
	unsigned char MyPower;
	if (!IsWirePowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower))
	{
		m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, 0);
		return;
	}

	m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);

	if (MyPower < 1)
	{
		return;
	}

	MyPower--;

	for (size_t i = 0; i < ARRAYCOUNT(gCrossCoords); i++)  // Loop through all directions to transfer or receive power
	{
		if ((i >= 4) && (i <= 7))  // If we are currently checking for wire surrounding ourself one block above...
		{
			BLOCKTYPE Type = 0;
			if (a_RelBlockY + 1 >= cChunkDef::Height)
			{
				continue;
			}
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ, Type))
			{
				continue;
			}
			if (cBlockInfo::IsSolid(Type))  // If there is something solid above us (wire cut off)...
			{
				continue;  // We don't receive power from that wire
			}
		}
		else if ((i >= 8) && (i <= 11))  // See above, but this is for wire below us
		{
			BLOCKTYPE Type = 0;
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX + gCrossCoords[i].x, a_RelBlockY, a_RelBlockZ + gCrossCoords[i].z, Type))
			{
				continue;
			}
			if (cBlockInfo::IsSolid(Type))
			{
				continue;
			}
		}

		BLOCKTYPE Type = 0;
		if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX + gCrossCoords[i].x, a_RelBlockY + gCrossCoords[i].y, a_RelBlockZ + gCrossCoords[i].z, Type))
		{
			continue;
		}
		if (Type == E_BLOCK_REDSTONE_WIRE)
		{
			SetBlockPowered(a_RelBlockX + gCrossCoords[i].x, a_RelBlockY + gCrossCoords[i].y, a_RelBlockZ + gCrossCoords[i].z, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
		}
	}

	for (size_t i = 0; i < ARRAYCOUNT(gSideCoords); i++)  // Look for repeaters immediately surrounding self and try to power them
	{
		BLOCKTYPE Type = 0;
		if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX + gSideCoords[i].x, a_RelBlockY + gSideCoords[i].y, a_RelBlockZ + gSideCoords[i].z, Type))
		{
			continue;
		}
		if (Type == E_BLOCK_REDSTONE_REPEATER_OFF)
		{
			SetBlockPowered(a_RelBlockX + gSideCoords[i].x, a_RelBlockY + gSideCoords[i].y, a_RelBlockZ + gSideCoords[i].z, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
		}
	}

	// Wire still powered, power blocks beneath
	SetBlockPowered(a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
	SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_YM, MyPower);

	switch (GetWireDirection(a_RelBlockX, a_RelBlockY, a_RelBlockZ))
	{
		case REDSTONE_NONE:
		{
			SetBlockPowered(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
			SetBlockPowered(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
			SetBlockPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
			SetBlockPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);

			SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_XM, MyPower);
			SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_XP, MyPower);
			SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_ZM, MyPower);
			SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_ZP, MyPower);
			break;
		}
		case REDSTONE_X_POS:
		{
			SetBlockPowered(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
			SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_XP, MyPower);
			break;
		}
		case REDSTONE_X_NEG:
		{
			SetBlockPowered(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
			SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_XM, MyPower);
			break;
		}
		case REDSTONE_Z_POS:
		{
			SetBlockPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
			SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_ZP, MyPower);
			break;
		}
		case REDSTONE_Z_NEG:
		{
			SetBlockPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MyPower);
			SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_ZM, MyPower);
			break;
		}
	}
}





void cIncrementalRedstoneSimulator::HandleRedstoneRepeater(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, BLOCKTYPE a_MyState)
{
	/* Repeater Orientation Mini Guide:
	===================================

	|
	| Z Axis
	V

	X Axis ---->

	Repeater directions, values from a WorldType::GetBlockMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) lookup:

	East (Right) (X+): 0x1
	West (Left) (X-): 0x3
	North (Up) (Z-): 0x2
	South (Down) (Z+): 0x0
	// TODO: Add E_META_XXX enum entries for all meta values and update project with them

	Sun rises from East (X+)

	*/

	// Create a variable holding my meta to avoid multiple lookups.
	NIBBLETYPE a_Meta = m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
	bool IsOn = (a_MyState == E_BLOCK_REDSTONE_REPEATER_ON);

	if (!IsRepeaterLocked(a_RelBlockX, a_RelBlockY, a_RelBlockZ, a_Meta))  // If we're locked, change nothing. Otherwise:
	{
		bool IsSelfPowered = IsRepeaterPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, a_Meta);
		if (IsSelfPowered && !IsOn)  // Queue a power change if powered, but not on and not locked.
		{
			QueueRepeaterPowerChange(a_RelBlockX, a_RelBlockY, a_RelBlockZ, a_Meta, true);
		}
		else if (!IsSelfPowered && IsOn)  // Queue a power change if unpowered, on, and not locked.
		{
			QueueRepeaterPowerChange(a_RelBlockX, a_RelBlockY, a_RelBlockZ, a_Meta, false);
		}
	}
}


void cIncrementalRedstoneSimulator::HandleRedstoneRepeaterDelays()
{
	for (auto itr = m_RepeatersDelayList->begin(); itr != m_RepeatersDelayList->end();)
	{
		if (itr->a_ElapsedTicks >= itr->a_DelayTicks)  // Has the elapsed ticks reached the target ticks?
		{
			int RelBlockX = itr->a_RelBlockPos.x;
			int RelBlockY = itr->a_RelBlockPos.y;
			int RelBlockZ = itr->a_RelBlockPos.z;
			BLOCKTYPE Block;
			NIBBLETYPE Meta;
			m_Chunk->GetBlockTypeMeta(RelBlockX, RelBlockY, RelBlockZ, Block, Meta);
			if (itr->ShouldPowerOn)
			{
				if (Block != E_BLOCK_REDSTONE_REPEATER_ON)  // For performance
				{
					m_Chunk->SetBlock(itr->a_RelBlockPos, E_BLOCK_REDSTONE_REPEATER_ON, Meta);
				}

				switch (Meta & 0x3)  // We only want the direction (bottom) bits
				{
					case 0x0:
					{
						SetBlockPowered(RelBlockX, RelBlockY, RelBlockZ - 1, RelBlockX, RelBlockY, RelBlockZ);
						SetDirectionLinkedPowered(RelBlockX, RelBlockY, RelBlockZ, BLOCK_FACE_ZM);
						break;
					}
					case 0x1:
					{
						SetBlockPowered(RelBlockX + 1, RelBlockY, RelBlockZ, RelBlockX, RelBlockY, RelBlockZ);
						SetDirectionLinkedPowered(RelBlockX, RelBlockY, RelBlockZ, BLOCK_FACE_XP);
						break;
					}
					case 0x2:
					{
						SetBlockPowered(RelBlockX, RelBlockY, RelBlockZ + 1, RelBlockX, RelBlockY, RelBlockZ);
						SetDirectionLinkedPowered(RelBlockX, RelBlockY, RelBlockZ, BLOCK_FACE_ZP);
						break;
					}
					case 0x3:
					{
						SetBlockPowered(RelBlockX - 1, RelBlockY, RelBlockZ, RelBlockX, RelBlockY, RelBlockZ);
						SetDirectionLinkedPowered(RelBlockX, RelBlockY, RelBlockZ, BLOCK_FACE_XM);
						break;
					}
				}
			}
			else if (Block != E_BLOCK_REDSTONE_REPEATER_OFF)
			{
				m_Chunk->SetBlock(RelBlockX, RelBlockY, RelBlockZ, E_BLOCK_REDSTONE_REPEATER_OFF, Meta);
			}
			itr = m_RepeatersDelayList->erase(itr);
		}
		else
		{
			LOGD("Incremented a repeater @ {%i %i %i} | Elapsed ticks: %i | Target delay: %i", itr->a_RelBlockPos.x, itr->a_RelBlockPos.y, itr->a_RelBlockPos.z, itr->a_ElapsedTicks, itr->a_DelayTicks);
			itr->a_ElapsedTicks++;
			itr++;
		}
	}
}





void cIncrementalRedstoneSimulator::HandlePiston(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
	int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;

	if (IsPistonPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) & 0x7))  // We only want the bottom three bits (4th controls extended-ness)
	{
		GetHandlerCompileTime<E_BLOCK_PISTON>::type::ExtendPiston(BlockX, a_RelBlockY, BlockZ, &this->m_World);
	}
	else
	{
		GetHandlerCompileTime<E_BLOCK_PISTON>::type::RetractPiston(BlockX, a_RelBlockY, BlockZ, &this->m_World);
	}
}





void cIncrementalRedstoneSimulator::HandleDropSpenser(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	class cSetPowerToDropSpenser :
		public cRedstonePoweredCallback
	{
		bool m_IsPowered;
	public:
		cSetPowerToDropSpenser(bool a_IsPowered) : m_IsPowered(a_IsPowered) {}

		virtual bool Item(cRedstonePoweredEntity * a_DropSpenser) override
		{
			a_DropSpenser->SetRedstonePower(m_IsPowered);
			return false;
		}
	} DrSpSP(AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ));

	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
	int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;
	m_Chunk->DoWithRedstonePoweredEntityAt(BlockX, a_RelBlockY, BlockZ, DrSpSP);
}





void cIncrementalRedstoneSimulator::HandleRedstoneLamp(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, BLOCKTYPE a_MyState)
{
	if (a_MyState == E_BLOCK_REDSTONE_LAMP_OFF)
	{
		if (AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ))
		{
			m_Chunk->SetBlock(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_BLOCK_REDSTONE_LAMP_ON, 0);
		}
	}
	else
	{
		if (!AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ))
		{
			m_Chunk->SetBlock(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_BLOCK_REDSTONE_LAMP_OFF, 0);
		}
	}
}





void cIncrementalRedstoneSimulator::HandleTNT(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
	int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;

	if (AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ))
	{
		m_Chunk->BroadcastSoundEffect("game.tnt.primed", (double)BlockX, (double)a_RelBlockY, (double)BlockZ, 0.5f, 0.6f);
		m_Chunk->SetBlock(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_BLOCK_AIR, 0);
		this->m_World.SpawnPrimedTNT(BlockX + 0.5, a_RelBlockY + 0.5, BlockZ + 0.5);  // 80 ticks to boom
	}
}





void cIncrementalRedstoneSimulator::HandleDoor(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
	int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;

	typedef GetHandlerCompileTime<E_BLOCK_WOODEN_DOOR>::type DoorHandler;

	if (AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ))
	{
		if (!AreCoordsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, true))
		{
			cChunkInterface ChunkInterface(this->m_World.GetChunkMap());
			if (!DoorHandler::IsOpen(ChunkInterface, BlockX, a_RelBlockY, BlockZ))
			{
				DoorHandler::SetOpen(ChunkInterface, BlockX, a_RelBlockY, BlockZ, true);
				m_Chunk->BroadcastSoundParticleEffect(1003, BlockX, a_RelBlockY, BlockZ, 0);
			}
			SetPlayerToggleableBlockAsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, true);
		}
	}
	else
	{
		if (!AreCoordsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, false))
		{
			cChunkInterface ChunkInterface(this->m_World.GetChunkMap());
			if (DoorHandler::IsOpen(ChunkInterface, BlockX, a_RelBlockY, BlockZ))
			{
				DoorHandler::SetOpen(ChunkInterface, BlockX, a_RelBlockY, BlockZ, false);
				m_Chunk->BroadcastSoundParticleEffect(1003, BlockX, a_RelBlockY, BlockZ, 0);
			}
			SetPlayerToggleableBlockAsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, false);
		}
	}
}





void cIncrementalRedstoneSimulator::HandleCommandBlock(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	class cSetPowerToCommandBlock :
		public cRedstonePoweredCallback
	{
		bool m_IsPowered;
	public:
		cSetPowerToCommandBlock(bool a_IsPowered) : m_IsPowered(a_IsPowered) {}

		virtual bool Item(cRedstonePoweredEntity * a_CommandBlock) override
		{
			a_CommandBlock->SetRedstonePower(m_IsPowered);
			return false;
		}
	} CmdBlockSP(AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ));

	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
	int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;
	m_Chunk->DoWithRedstonePoweredEntityAt(BlockX, a_RelBlockY, BlockZ, CmdBlockSP);
}





void cIncrementalRedstoneSimulator::HandleRail(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, BLOCKTYPE a_MyType)
{
	switch (a_MyType)
	{
		case E_BLOCK_DETECTOR_RAIL:
		{
			if ((m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) & 0x08) == 0x08)
			{
				SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, a_MyType);
			}
			break;
		}
		case E_BLOCK_ACTIVATOR_RAIL:
		case E_BLOCK_POWERED_RAIL:
		{
			if (AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ))
			{
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) | 0x08);
			}
			else
			{
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) & 0x07);
			}
			break;
		}
		default: LOGD("Unhandled type of rail in %s", __FUNCTION__);
	}
}





void cIncrementalRedstoneSimulator::HandleTrapdoor(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
	int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;

	if (AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ))
	{
		if (!AreCoordsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, true))
		{
			this->m_World.SetTrapdoorOpen(BlockX, a_RelBlockY, BlockZ, true);
			SetPlayerToggleableBlockAsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, true);
		}
	}
	else
	{
		if (!AreCoordsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, false))
		{
			this->m_World.SetTrapdoorOpen(BlockX, a_RelBlockY, BlockZ, false);
			SetPlayerToggleableBlockAsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, false);
		}
	}
}





void cIncrementalRedstoneSimulator::HandleNoteBlock(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	bool m_bAreCoordsPowered = AreCoordsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ);

	if (m_bAreCoordsPowered)
	{
		if (!AreCoordsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, true))
		{
			class cSetPowerToNoteBlock :
				public cRedstonePoweredCallback
			{
			public:
				cSetPowerToNoteBlock() {}

				virtual bool Item(cRedstonePoweredEntity * a_NoteBlock) override
				{
					a_NoteBlock->SetRedstonePower(true);
					return false;
				}
			} NoteBlockSP;

			int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
			int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;
			m_Chunk->DoWithRedstonePoweredEntityAt(BlockX, a_RelBlockY, BlockZ, NoteBlockSP);
			SetPlayerToggleableBlockAsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, true);
		}
	}
	else
	{
		if (!AreCoordsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, false))
		{
			SetPlayerToggleableBlockAsSimulated(a_RelBlockX, a_RelBlockY, a_RelBlockZ, false);
		}
	}
}





void cIncrementalRedstoneSimulator::HandleDaylightSensor(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX, BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;
	int ChunkX, ChunkZ;
	cChunkDef::BlockToChunk(BlockX, BlockZ, ChunkX, ChunkZ);

	if (!this->m_World.IsChunkLighted(ChunkX, ChunkZ))
	{
		this->m_World.QueueLightChunk(ChunkX, ChunkZ);
	}
	else
	{
		if (m_Chunk->GetTimeAlteredLight(this->m_World.GetBlockSkyLight(BlockX, a_RelBlockY + 1, BlockZ)) > 8)
		{
			SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
		}
		else
		{
			WakeUp(BlockX, a_RelBlockY, BlockZ, m_Chunk);
		}
	}
}





void cIncrementalRedstoneSimulator::HandlePressurePlate(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, BLOCKTYPE a_MyType)
{
	int BlockX = (m_Chunk->GetPosX() * cChunkDef::Width) + a_RelBlockX;
	int BlockZ = (m_Chunk->GetPosZ() * cChunkDef::Width) + a_RelBlockZ;

	switch (a_MyType)
	{
		case E_BLOCK_STONE_PRESSURE_PLATE:
		{
			// MCS feature - stone pressure plates can only be triggered by players :D
			cPlayer * a_Player = this->m_World.FindClosestPlayer(Vector3f(BlockX + 0.5f, (float)a_RelBlockY, BlockZ + 0.5f), 0.5f, false);

			if (a_Player != nullptr)
			{
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, 0x1);
				SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
				SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_YM, a_MyType);
			}
			else
			{
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, 0x0);
				SetSourceUnpowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk);
			}
			break;
		}
		case E_BLOCK_LIGHT_WEIGHTED_PRESSURE_PLATE:
		{
			class cPressurePlateCallback :
				public cEntityCallback
			{
			public:
				cPressurePlateCallback(int a_BlockX, int a_BlockY, int a_BlockZ) :
					m_NumberOfEntities(0),
					m_X(a_BlockX),
					m_Y(a_BlockY),
					m_Z(a_BlockZ)
				{
				}

				virtual bool Item(cEntity * a_Entity) override
				{
					Vector3f EntityPos = a_Entity->GetPosition();
					Vector3f BlockPos(m_X + 0.5f, (float)m_Y, m_Z + 0.5f);
					double Distance = (EntityPos - BlockPos).Length();

					if (Distance <= 0.5)
					{
						m_NumberOfEntities++;
					}
					return false;
				}

				bool GetPowerLevel(unsigned char & a_PowerLevel) const
				{
					a_PowerLevel = std::min(m_NumberOfEntities, MAX_POWER_LEVEL);
					return (a_PowerLevel > 0);
				}

			protected:
				int m_NumberOfEntities;

				int m_X;
				int m_Y;
				int m_Z;
			};

			cPressurePlateCallback PressurePlateCallback(BlockX, a_RelBlockY, BlockZ);
			this->m_World.ForEachEntityInChunk(m_Chunk->GetPosX(), m_Chunk->GetPosZ(), PressurePlateCallback);

			unsigned char Power;
			NIBBLETYPE Meta = m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
			if (PressurePlateCallback.GetPowerLevel(Power))
			{
				if (Meta == E_META_PRESSURE_PLATE_RAISED)
				{
					m_Chunk->BroadcastSoundEffect("random.click", (double)BlockX + 0.5, (double)a_RelBlockY + 0.1, (double)BlockZ + 0.5, 0.3F, 0.5F);
				}
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_META_PRESSURE_PLATE_DEPRESSED);
				SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, Power);
				SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_YM, a_MyType);
			}
			else
			{
				if (Meta == E_META_PRESSURE_PLATE_DEPRESSED)
				{
					m_Chunk->BroadcastSoundEffect("random.click", (double)BlockX + 0.5, (double)a_RelBlockY + 0.1, (double)BlockZ + 0.5, 0.3F, 0.6F);
				}
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_META_PRESSURE_PLATE_RAISED);
				SetSourceUnpowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk);
			}

			break;
		}
		case E_BLOCK_HEAVY_WEIGHTED_PRESSURE_PLATE:
		{
			class cPressurePlateCallback :
				public cEntityCallback
			{
			public:
				cPressurePlateCallback(int a_BlockX, int a_BlockY, int a_BlockZ) :
					m_NumberOfEntities(0),
					m_X(a_BlockX),
					m_Y(a_BlockY),
					m_Z(a_BlockZ)
				{
				}

				virtual bool Item(cEntity * a_Entity) override
				{
					Vector3f EntityPos = a_Entity->GetPosition();
					Vector3f BlockPos(m_X + 0.5f, (float)m_Y, m_Z + 0.5f);
					double Distance = (EntityPos - BlockPos).Length();

					if (Distance <= 0.5)
					{
						m_NumberOfEntities++;
					}
					return false;
				}

				bool GetPowerLevel(unsigned char & a_PowerLevel) const
				{
					a_PowerLevel = std::min((int)ceil(m_NumberOfEntities / 10.f), MAX_POWER_LEVEL);
					return (a_PowerLevel > 0);
				}

			protected:
				int m_NumberOfEntities;

				int m_X;
				int m_Y;
				int m_Z;
			};

			cPressurePlateCallback PressurePlateCallback(BlockX, a_RelBlockY, BlockZ);
			this->m_World.ForEachEntityInChunk(m_Chunk->GetPosX(), m_Chunk->GetPosZ(), PressurePlateCallback);

			unsigned char Power;
			NIBBLETYPE Meta = m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
			if (PressurePlateCallback.GetPowerLevel(Power))
			{
				if (Meta == E_META_PRESSURE_PLATE_RAISED)
				{
					m_Chunk->BroadcastSoundEffect("random.click", (double)BlockX + 0.5, (double)a_RelBlockY + 0.1, (double)BlockZ + 0.5, 0.3F, 0.5F);
				}
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_META_PRESSURE_PLATE_DEPRESSED);
				SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, Power);
				SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_YM, a_MyType);
			}
			else
			{
				if (Meta == E_META_PRESSURE_PLATE_DEPRESSED)
				{
					m_Chunk->BroadcastSoundEffect("random.click", (double)BlockX + 0.5, (double)a_RelBlockY + 0.1, (double)BlockZ + 0.5, 0.3F, 0.6F);
				}
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_META_PRESSURE_PLATE_RAISED);
				SetSourceUnpowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk);
			}

			break;
		}
		case E_BLOCK_WOODEN_PRESSURE_PLATE:
		{
			class cPressurePlateCallback :
				public cEntityCallback
			{
			public:
				cPressurePlateCallback(int a_BlockX, int a_BlockY, int a_BlockZ) :
					m_FoundEntity(false),
					m_X(a_BlockX),
					m_Y(a_BlockY),
					m_Z(a_BlockZ)
				{
				}

				virtual bool Item(cEntity * a_Entity) override
				{
					Vector3f EntityPos = a_Entity->GetPosition();
					Vector3f BlockPos(m_X + 0.5f, (float)m_Y, m_Z + 0.5f);
					double Distance = (EntityPos - BlockPos).Length();

					if (Distance <= 0.5)
					{
						m_FoundEntity = true;
						return true;  // Break out, we only need to know for plates that at least one entity is on top
					}
					return false;
				}

				bool FoundEntity(void) const
				{
					return m_FoundEntity;
				}

			protected:
				bool m_FoundEntity;

				int m_X;
				int m_Y;
				int m_Z;
			};

			cPressurePlateCallback PressurePlateCallback(BlockX, a_RelBlockY, BlockZ);
			this->m_World.ForEachEntityInChunk(m_Chunk->GetPosX(), m_Chunk->GetPosZ(), PressurePlateCallback);

			NIBBLETYPE Meta = m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
			if (PressurePlateCallback.FoundEntity())
			{
				if (Meta == E_META_PRESSURE_PLATE_RAISED)
				{
					m_Chunk->BroadcastSoundEffect("random.click", (double)BlockX + 0.5, (double)a_RelBlockY + 0.1, (double)BlockZ + 0.5, 0.3F, 0.5F);
				}
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_META_PRESSURE_PLATE_DEPRESSED);
				SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
				SetDirectionLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, BLOCK_FACE_YM, a_MyType);
			}
			else
			{
				if (Meta == E_META_PRESSURE_PLATE_DEPRESSED)
				{
					m_Chunk->BroadcastSoundEffect("random.click", (double)BlockX + 0.5, (double)a_RelBlockY + 0.1, (double)BlockZ + 0.5, 0.3F, 0.6F);
				}
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, E_META_PRESSURE_PLATE_RAISED);
				SetSourceUnpowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk);
			}
			break;
		}
		default:
		{
			LOGD("Unimplemented pressure plate type %s in cRedstoneSimulator", ItemToFullString(cItem(a_MyType)).c_str());
			break;
		}
	}
}





void cIncrementalRedstoneSimulator::HandleTripwireHook(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int RelX = a_RelBlockX, RelZ = a_RelBlockZ;
	bool FoundActivated = false;
	eBlockFace FaceToGoTowards = GetHandlerCompileTime<E_BLOCK_TRIPWIRE_HOOK>::type::MetadataToDirection(m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ));

	for (int i = 0; i < 40; ++i)  // Tripwires can be connected up to 40 blocks
	{
		BLOCKTYPE Type;
		NIBBLETYPE Meta;

		AddFaceDirection(RelX, a_RelBlockY, RelZ, FaceToGoTowards);
		m_Chunk->UnboundedRelGetBlock(RelX, a_RelBlockY, RelZ, Type, Meta);

		if (Type == E_BLOCK_TRIPWIRE)
		{
			if (Meta == 0x1)
			{
				FoundActivated = true;
			}
		}
		else if (Type == E_BLOCK_TRIPWIRE_HOOK)
		{
			if (ReverseBlockFace(GetHandlerCompileTime<E_BLOCK_TRIPWIRE_HOOK>::type::MetadataToDirection(Meta)) == FaceToGoTowards)
			{
				// Other hook facing in opposite direction - circuit completed!
				break;
			}
			else
			{
				// Tripwire hook not connected at all, AND away all the power state bits
				m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) & 0x3);
				SetSourceUnpowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk);
				return;
			}
		}
		else
		{
			// Tripwire hook not connected at all, AND away all the power state bits
			m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) & 0x3);
			SetSourceUnpowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk);
			return;
		}
	}

	if (FoundActivated)
	{
		// Connected and activated, set the 3rd and 4th highest bits
		m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) | 0xC);
		SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
	}
	else
	{
		// Connected but not activated, AND away the highest bit
		m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, (m_Chunk->GetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ) & 0x7) | 0x4);
		SetSourceUnpowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk);
	}
}





void cIncrementalRedstoneSimulator::HandleTrappedChest(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	class cGetTrappedChestPlayers :
		public cItemCallback<cChestEntity>
	{
	public:
		cGetTrappedChestPlayers(void) :
			m_NumberOfPlayers(0)
		{
		}

		virtual ~cGetTrappedChestPlayers()
		{
		}

		virtual bool Item(cChestEntity * a_Chest) override
		{
			ASSERT(a_Chest->GetBlockType() == E_BLOCK_TRAPPED_CHEST);
			m_NumberOfPlayers = a_Chest->GetNumberOfPlayers();
			return (m_NumberOfPlayers <= 0);
		}

		unsigned char GetPowerLevel(void) const
		{
			return std::min(m_NumberOfPlayers, MAX_POWER_LEVEL);
		}

	private:
		int m_NumberOfPlayers;

	} GTCP;

	int BlockX = m_Chunk->GetPosX() * cChunkDef::Width + a_RelBlockX;
	int BlockZ = m_Chunk->GetPosZ() * cChunkDef::Width + a_RelBlockZ;
	if (m_Chunk->DoWithChestAt(BlockX, a_RelBlockY, BlockZ, GTCP))
	{
		SetAllDirsAsPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, GTCP.GetPowerLevel());
	}
	else
	{
		SetSourceUnpowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ, m_Chunk);
	}
}





void cIncrementalRedstoneSimulator::HandleTripwire(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int BlockX = m_Chunk->GetPosX() * cChunkDef::Width + a_RelBlockX;
	int BlockZ = m_Chunk->GetPosZ() * cChunkDef::Width + a_RelBlockZ;

	class cTripwireCallback :
		public cEntityCallback
	{
	public:
		cTripwireCallback(int a_BlockX, int a_BlockY, int a_BlockZ) :
			m_FoundEntity(false),
			m_X(a_BlockX),
			m_Y(a_BlockY),
			m_Z(a_BlockZ)
		{
		}

		virtual bool Item(cEntity * a_Entity) override
		{
			cBoundingBox bbWire(m_X, m_X + 1, m_Y, m_Y + 0.1, m_Z, m_Z + 1);
			cBoundingBox bbEntity(a_Entity->GetPosition(), a_Entity->GetWidth() / 2, a_Entity->GetHeight());

			if (bbEntity.DoesIntersect(bbWire))
			{
				m_FoundEntity = true;
				return true;  // One entity is sufficient to trigger the wire
			}
			return false;
		}

		bool FoundEntity(void) const
		{
			return m_FoundEntity;
		}

	protected:
		bool m_FoundEntity;

		int m_X;
		int m_Y;
		int m_Z;
	};

	cTripwireCallback TripwireCallback(BlockX, a_RelBlockY, BlockZ);
	this->m_World.ForEachEntityInChunk(m_Chunk->GetPosX(), m_Chunk->GetPosZ(), TripwireCallback);

	if (TripwireCallback.FoundEntity())
	{
		m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, 0x1);
	}
	else
	{
		m_Chunk->SetMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, 0x0);
	}
}





bool cIncrementalRedstoneSimulator::AreCoordsDirectlyPowered(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, cChunk * a_Chunk)
{
	// Torches want to access neighbour's data when on a wall, hence the extra chunk parameter

	for (const auto & itr : ((cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData())->m_PoweredBlocks)  // Check powered list
	{
		if (itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			return true;
		}
	}
	return false;
}





bool cIncrementalRedstoneSimulator::AreCoordsLinkedPowered(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	for (const auto & itr : *m_LinkedPoweredBlocks)  // Check linked powered list
	{
		if (itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			return true;
		}
	}
	return false;
}





bool cIncrementalRedstoneSimulator::IsRepeaterPowered(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, NIBBLETYPE a_Meta)
{
	// Repeaters cannot be powered by any face except their back; verify that this is true for a source

	for (const auto & itr : *m_PoweredBlocks)
	{
		if (!itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			continue;
		}

		switch (a_Meta & 0x3)
		{
			case 0x0:
			{
				// Flip the coords to check the back of the repeater
				if (itr.a_SourcePos.Equals(AdjustRelativeCoords(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1))))
				{
					return true;
				}
				break;
			}
			case 0x1:
			{
				if (itr.a_SourcePos.Equals(AdjustRelativeCoords(Vector3i(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ))))
				{
					return true;
				}
				break;
			}
			case 0x2:
			{
				if (itr.a_SourcePos.Equals(AdjustRelativeCoords(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1))))
				{
					return true;
				}
				break;
			}
			case 0x3:
			{
				if (itr.a_SourcePos.Equals(AdjustRelativeCoords(Vector3i(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ))))
				{
					return true;
				}
				break;
			}
		}
	}  // for itr - m_PoweredBlocks[]

	for (const auto & itr : *m_LinkedPoweredBlocks)
	{
		if (!itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			continue;
		}

		switch (a_Meta & 0x3)
		{
			case 0x0:
			{
				if (itr.a_MiddlePos.Equals(AdjustRelativeCoords(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1))))
				{
					return true;
				}
				break;
			}
			case 0x1:
			{
				if (itr.a_MiddlePos.Equals(AdjustRelativeCoords(Vector3i(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ))))
				{
					return true;
				}
				break;
			}
			case 0x2:
			{
				if (itr.a_MiddlePos.Equals(AdjustRelativeCoords(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1))))
				{
					return true;
				}
				break;
			}
			case 0x3:
			{
				if (itr.a_MiddlePos.Equals(AdjustRelativeCoords(Vector3i(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ))))
				{
					return true;
				}
				break;
			}
		}
	}  // for itr - m_LinkedPoweredBlocks[]
	return false;  // Couldn't find power source behind repeater
}





bool cIncrementalRedstoneSimulator::IsRepeaterLocked(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, NIBBLETYPE a_Meta)
{
	switch (a_Meta & 0x3)  // We only want the 'direction' part of our metadata
	{
		// If the repeater is looking up or down (If parallel to the Z axis)
		case 0x0:
		case 0x2:
		{
			// Check if eastern (right) neighbor is a powered on repeater who is facing us
			BLOCKTYPE Block = 0;
			NIBBLETYPE OtherRepeaterDir = 0;
			if (
				m_Chunk->UnboundedRelGetBlock(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, Block, OtherRepeaterDir) &&
				(Block == E_BLOCK_REDSTONE_REPEATER_ON)
			)
			{
				if ((OtherRepeaterDir & 0x03) == 0x3)
				{
					return true;
				}  // If so, I am latched/locked
			}

			// Check if western(left) neighbor is a powered on repeater who is facing us
			if (
				m_Chunk->UnboundedRelGetBlock(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, Block, OtherRepeaterDir) &&
				(Block == E_BLOCK_REDSTONE_REPEATER_ON)
			)
			{
				if ((OtherRepeaterDir & 0x03) == 0x1)
				{
					return true;
				}  // If so, I am latched/locked
			}

			break;
		}

		// If the repeater is looking left or right (If parallel to the x axis)
		case 0x1:
		case 0x3:
		{
			// Check if southern(down) neighbor is a powered on repeater who is facing us
			BLOCKTYPE Block = 0;
			NIBBLETYPE OtherRepeaterDir = 0;

			if (
				m_Chunk->UnboundedRelGetBlock(a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, Block, OtherRepeaterDir) &&
				(Block == E_BLOCK_REDSTONE_REPEATER_ON)
			)
			{
				if ((OtherRepeaterDir & 0x30) == 0x00)
				{
					return true;
				}  // If so,  am latched/locked
			}

			// Check if northern(up) neighbor is a powered on repeater who is facing us
			if (
				m_Chunk->UnboundedRelGetBlock(a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, Block, OtherRepeaterDir) &&
				(Block == E_BLOCK_REDSTONE_REPEATER_ON)
			)
			{
				if ((OtherRepeaterDir & 0x03) == 0x02)
				{
					return true;
				}  // If so, I am latched/locked
			}

			break;
		}
	}

	return false;  // None of the checks succeeded, I am not a locked repeater
}




bool cIncrementalRedstoneSimulator::IsPistonPowered(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, NIBBLETYPE a_Meta)
{
	// Pistons cannot be powered through their front face; this function verifies that a source meets this requirement

	eBlockFace Face = GetHandlerCompileTime<E_BLOCK_PISTON>::type::MetaDataToDirection(a_Meta);

	for (const auto & itr : *m_PoweredBlocks)
	{
		if (!itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			continue;
		}

		int X = a_RelBlockX, Z = a_RelBlockZ;
		AddFaceDirection(X, a_RelBlockY, Z, Face);

		if (!itr.a_SourcePos.Equals(AdjustRelativeCoords(Vector3i(X, a_RelBlockY, Z))))
		{
			return true;
		}
	}

	for (const auto & itr : *m_LinkedPoweredBlocks)
	{
		if (!itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			continue;
		}

		int X = a_RelBlockX, Z = a_RelBlockZ;
		AddFaceDirection(X, a_RelBlockY, Z, Face);

		if (!itr.a_MiddlePos.Equals(AdjustRelativeCoords(Vector3i(X, a_RelBlockY, Z))))
		{
			return true;
		}
	}
	return false;  // Source was in front of the piston's front face
}




bool cIncrementalRedstoneSimulator::IsWirePowered(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, unsigned char & a_PowerLevel)
{
	a_PowerLevel = 0;

	for (const auto & itr : *m_PoweredBlocks)  // Check powered list
	{
		if (!itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			continue;
		}
		a_PowerLevel = std::max(itr.a_PowerLevel, a_PowerLevel);  // Get the highest power level (a_PowerLevel is initialised already and there CAN be multiple levels for one block)
	}

	for (const auto & itr : *m_LinkedPoweredBlocks)  // Check linked powered list
	{
		if (!itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			continue;
		}

		BLOCKTYPE Type = E_BLOCK_AIR;
		if (!m_Chunk->UnboundedRelGetBlockType(itr.a_SourcePos.x, itr.a_SourcePos.y, itr.a_SourcePos.z, Type) || (Type == E_BLOCK_REDSTONE_WIRE))
		{
			continue;
		}
		a_PowerLevel = std::max(itr.a_PowerLevel, a_PowerLevel);
	}

	return (a_PowerLevel != 0);  // Answer the inital question: is the wire powered?
}





bool cIncrementalRedstoneSimulator::AreCoordsSimulated(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, bool IsCurrentStatePowered)
{
	for (const auto & itr : *m_SimulatedPlayerToggleableBlocks)
	{
		if (itr.a_RelBlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			if (itr.WasLastStatePowered != IsCurrentStatePowered)  // Was the last power state different to the current?
			{
				return false;  // It was, coordinates are no longer simulated
			}
			else
			{
				return true;  // It wasn't, don't resimulate block, and allow players to toggle
			}
		}
	}
	return false;  // Block wasn't even in the list, not simulated
}





void cIncrementalRedstoneSimulator::SetDirectionLinkedPowered(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, char a_Direction, unsigned char a_PowerLevel)
{
	BLOCKTYPE MiddleBlock = 0;
	switch (a_Direction)
	{
		case BLOCK_FACE_XM:
		{
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, MiddleBlock))
			{
				return;
			}

			SetBlockLinkedPowered(a_RelBlockX - 2, a_RelBlockY, a_RelBlockZ, a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX - 1, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX - 1, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);

			break;
		}
		case BLOCK_FACE_XP:
		{
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, MiddleBlock))
			{
				return;
			}

			SetBlockLinkedPowered(a_RelBlockX + 2, a_RelBlockY, a_RelBlockZ, a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX + 1, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX + 1, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);

			break;
		}
		case BLOCK_FACE_YM:
		{
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ, MiddleBlock))
			{
				return;
			}

			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY - 2, a_RelBlockZ, a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX + 1, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX - 1, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);

			break;
		}
		case BLOCK_FACE_YP:
		{
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ, MiddleBlock))
			{
				return;
			}

			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY + 2, a_RelBlockZ, a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX + 1, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX - 1, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);

			break;
		}
		case BLOCK_FACE_ZM:
		{
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, MiddleBlock))
			{
				return;
			}

			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ - 2, a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);

			break;
		}
		case BLOCK_FACE_ZP:
		{
			if (!m_Chunk->UnboundedRelGetBlockType(a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, MiddleBlock))
			{
				return;
			}

			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY, a_RelBlockZ + 2, a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY + 1, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);
			SetBlockLinkedPowered(a_RelBlockX, a_RelBlockY - 1, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, a_RelBlockX, a_RelBlockY, a_RelBlockZ, MiddleBlock, a_PowerLevel);

			break;
		}
		default:
		{
			ASSERT(!"Unhandled face direction when attempting to set blocks as linked powered!");  // Zombies, that wasn't supposed to happen...
			break;
		}
	}
}





void cIncrementalRedstoneSimulator::SetAllDirsAsPowered(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, unsigned char a_PowerLevel)
{
	static const struct
	{
		int x, y, z;
	} gCrossCoords[] =
	{
		{ 1, 0, 0 },
		{ -1, 0, 0 },
		{ 0, 0, 1 },
		{ 0, 0, -1 },
		{ 0, 1, 0 },
		{ 0, -1, 0 }
	};

	for (size_t i = 0; i < ARRAYCOUNT(gCrossCoords); i++)  // Loop through struct to power all directions
	{
		SetBlockPowered(a_RelBlockX + gCrossCoords[i].x, a_RelBlockY + gCrossCoords[i].y, a_RelBlockZ + gCrossCoords[i].z, a_RelBlockX, a_RelBlockY, a_RelBlockZ, a_PowerLevel);
	}
}





void cIncrementalRedstoneSimulator::SetBlockPowered(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, int a_RelSourceX, int a_RelSourceY, int a_RelSourceZ, unsigned char a_PowerLevel)
{
	cChunk * Neighbour = m_Chunk->GetRelNeighborChunkAdjustCoords(a_RelBlockX, a_RelBlockZ);  // Adjust coordinates for the later call using these values
	if ((Neighbour == nullptr) || !Neighbour->IsValid())
	{
		return;
	}

	auto & Powered = ((cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)Neighbour->GetRedstoneSimulatorData())->m_PoweredBlocks;  // We need to insert the value into the chunk who owns the block position
	for (auto itr = Powered.begin(); itr != Powered.end(); ++itr)
	{
		if (
			itr->a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)) &&
			itr->a_SourcePos.Equals(Vector3i(a_RelSourceX, a_RelSourceY, a_RelSourceZ))
			)
		{
			// Check for duplicates, update power level, don't add a new listing
			itr->a_PowerLevel = a_PowerLevel;
			return;
		}
	}

	// No need to get neighbouring chunk as we can guarantee that when something is powering us, the entry will be in our chunk
	for (auto itr = m_PoweredBlocks->begin(); itr != m_PoweredBlocks->end(); ++itr)
	{
		if (
			itr->a_BlockPos.Equals(Vector3i(a_RelSourceX, a_RelSourceY, a_RelSourceZ)) &&
			itr->a_SourcePos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)) &&
			(m_Chunk->GetBlock(a_RelSourceX, a_RelSourceY, a_RelSourceZ) == E_BLOCK_REDSTONE_WIRE)
			)
		{
			BLOCKTYPE Block;
			NIBBLETYPE Meta;
			Neighbour->GetBlockTypeMeta(a_RelBlockX, a_RelBlockY, a_RelBlockZ, Block, Meta);

			if (Block == E_BLOCK_REDSTONE_WIRE)
			{
				if (Meta < a_PowerLevel)
				{
					m_PoweredBlocks->erase(itr);  // Powering source with higher power level, allow it
					break;
				}
				else
				{
					// Powered wires try to power their source - don't let them!
					return;
				}
			}
		}
	}

	sPoweredBlocks RC;
	RC.a_BlockPos = Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
	RC.a_SourcePos = Vector3i(a_RelSourceX, a_RelSourceY, a_RelSourceZ);
	RC.a_PowerLevel = a_PowerLevel;
	Powered.emplace_back(RC);
	Neighbour->SetIsRedstoneDirty(true);
	m_Chunk->SetIsRedstoneDirty(true);
}





void cIncrementalRedstoneSimulator::SetBlockLinkedPowered(
	int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ,
	int a_RelMiddleX, int a_RelMiddleY, int a_RelMiddleZ,
	int a_RelSourceX, int a_RelSourceY, int a_RelSourceZ,
	BLOCKTYPE a_MiddleBlock, unsigned char a_PowerLevel
	)
{
	if (!IsViableMiddleBlock(a_MiddleBlock))
	{
		return;
	}

	cChunk * Neighbour = m_Chunk->GetRelNeighborChunkAdjustCoords(a_RelBlockX, a_RelBlockZ);
	m_Chunk->GetRelNeighborChunkAdjustCoords(a_RelMiddleX, a_RelMiddleZ);
	if ((Neighbour == nullptr) || !Neighbour->IsValid())
	{
		return;
	}

	auto & Linked = ((cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)Neighbour->GetRedstoneSimulatorData())->m_LinkedBlocks;
	for (auto & itr : Linked)  // Check linked powered list
	{
		if (
			itr.a_BlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)) &&
			itr.a_MiddlePos.Equals(Vector3i(a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ)) &&
			itr.a_SourcePos.Equals(Vector3i(a_RelSourceX, a_RelSourceY, a_RelSourceZ))
			)
		{
			// Check for duplicates, update power level, don't add a new listing
			itr.a_PowerLevel = a_PowerLevel;
			return;
		}
	}

	sLinkedPoweredBlocks RC;
	RC.a_BlockPos = Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
	RC.a_MiddlePos = Vector3i(a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ);
	RC.a_SourcePos = Vector3i(a_RelSourceX, a_RelSourceY, a_RelSourceZ);
	RC.a_PowerLevel = a_PowerLevel;
	Linked.emplace_back(RC);
	Neighbour->SetIsRedstoneDirty(true);
	m_Chunk->SetIsRedstoneDirty(true);
}





void cIncrementalRedstoneSimulator::SetPlayerToggleableBlockAsSimulated(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, bool WasLastStatePowered)
{
	for (auto itr = m_SimulatedPlayerToggleableBlocks->begin(); itr != m_SimulatedPlayerToggleableBlocks->end(); ++itr)
	{
		if (!itr->a_RelBlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			continue;
		}

		if (itr->WasLastStatePowered != WasLastStatePowered)
		{
			// If power states different, update listing
			itr->WasLastStatePowered = WasLastStatePowered;
			return;
		}
		else
		{
			// If states the same, just ignore
			return;
		}
	}

	// We have arrive here; no block must be in list - add one
	sSimulatedPlayerToggleableList RC;
	RC.a_RelBlockPos = Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ);
	RC.WasLastStatePowered = WasLastStatePowered;
	m_SimulatedPlayerToggleableBlocks->emplace_back(RC);
}





bool cIncrementalRedstoneSimulator::QueueRepeaterPowerChange(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ, NIBBLETYPE a_Meta, bool ShouldPowerOn)
{
	for (auto itr = m_RepeatersDelayList->begin(); itr != m_RepeatersDelayList->end(); ++itr)
	{
		if (itr->a_RelBlockPos.Equals(Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ)))
		{
			if (ShouldPowerOn == itr->ShouldPowerOn)  // We are queued already for the same thing, don't replace entry
			{
				return false;
			}

			// Already in here (normal to allow repeater to continue on powering and updating blocks in front) - just update info and quit
			itr->a_DelayTicks = (((a_Meta & 0xC) >> 0x2) + 1) * 2;  // See below for description
			itr->a_ElapsedTicks = 0;
			itr->ShouldPowerOn = ShouldPowerOn;
			return false;
		}
	}

	// Self not in list, add self to list
	sRepeatersDelayList RC;
	RC.a_RelBlockPos = Vector3i(a_RelBlockX, a_RelBlockY, a_RelBlockZ);

	// Gets the top two bits (delay time), shifts them into the lower two bits, and adds one (meta 0 = 1 tick; 1 = 2 etc.)
	// Multiply by 2 because in MCS, 1 redstone tick = 1 world tick, but in Vanilla, 1 redstone tick = 2 world ticks, and we need to maintain compatibility
	RC.a_DelayTicks = (((a_Meta & 0xC) >> 0x2) + 1) * 2;

	RC.a_ElapsedTicks = 0;
	RC.ShouldPowerOn = ShouldPowerOn;
	m_RepeatersDelayList->emplace_back(RC);
	return true;
}





void cIncrementalRedstoneSimulator::SetSourceUnpowered(int a_RelSourceX, int a_RelSourceY, int a_RelSourceZ, cChunk * a_Chunk, bool a_IsFirstCall)
{
	if (!a_IsFirstCall)  // The neighbouring chunks passed when this parameter is false may be invalid
	{
		if ((a_Chunk == nullptr) || !a_Chunk->IsValid())
		{
			return;
		}
	}

	std::vector<Vector3i> BlocksPotentiallyUnpowered;

	auto Data = (cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData();
	Data->m_PoweredBlocks.erase(std::remove_if(Data->m_PoweredBlocks.begin(), Data->m_PoweredBlocks.end(), [&BlocksPotentiallyUnpowered, a_Chunk, a_RelSourceX, a_RelSourceY, a_RelSourceZ](const sPoweredBlocks & itr)
		{
			if (itr.a_SourcePos.Equals(Vector3i(a_RelSourceX, a_RelSourceY, a_RelSourceZ)))
			{
				BlocksPotentiallyUnpowered.emplace_back(itr.a_BlockPos);
				a_Chunk->SetIsRedstoneDirty(true);
				return true;
			}
			return false;
		}
	), Data->m_PoweredBlocks.end());

	Data->m_LinkedBlocks.erase(std::remove_if(Data->m_LinkedBlocks.begin(), Data->m_LinkedBlocks.end(), [&BlocksPotentiallyUnpowered, a_Chunk, a_RelSourceX, a_RelSourceY, a_RelSourceZ](const sLinkedPoweredBlocks & itr)
		{
			if (itr.a_SourcePos.Equals(Vector3i(a_RelSourceX, a_RelSourceY, a_RelSourceZ)))
			{
				BlocksPotentiallyUnpowered.emplace_back(itr.a_BlockPos);
				a_Chunk->SetIsRedstoneDirty(true);
				return true;
			}
			return false;
		}
	), Data->m_LinkedBlocks.end());

	if (a_IsFirstCall && AreCoordsOnChunkBoundary(a_RelSourceX, a_RelSourceY, a_RelSourceZ))
	{
		// +- 2 to accomodate linked powered blocks
		SetSourceUnpowered(a_RelSourceX, a_RelSourceY, a_RelSourceZ, a_Chunk->GetRelNeighborChunk(a_RelSourceX - 2, a_RelSourceZ), false);
		SetSourceUnpowered(a_RelSourceX, a_RelSourceY, a_RelSourceZ, a_Chunk->GetRelNeighborChunk(a_RelSourceX + 2, a_RelSourceZ), false);
		SetSourceUnpowered(a_RelSourceX, a_RelSourceY, a_RelSourceZ, a_Chunk->GetRelNeighborChunk(a_RelSourceX, a_RelSourceZ - 2), false);
		SetSourceUnpowered(a_RelSourceX, a_RelSourceY, a_RelSourceZ, a_Chunk->GetRelNeighborChunk(a_RelSourceX, a_RelSourceZ + 2), false);
	}

	for (const auto & itr : BlocksPotentiallyUnpowered)
	{
		auto Neighbour = a_Chunk->GetRelNeighborChunk(itr.x, itr.z);
		if (!AreCoordsPowered(itr.x, itr.y, itr.z) && (Neighbour->GetBlock(itr) != E_BLOCK_REDSTONE_REPEATER_ON))
		{
			// Repeaters time themselves with regards to unpowering; ensure we don't do it for them
			SetSourceUnpowered(itr.x, itr.y, itr.z, Neighbour);
		}
	}
}





void cIncrementalRedstoneSimulator::SetInvalidMiddleBlock(int a_RelMiddleX, int a_RelMiddleY, int a_RelMiddleZ, cChunk * a_Chunk, bool a_IsFirstCall)
{
	if (!a_IsFirstCall)  // The neighbouring chunks passed when this parameter is false may be invalid
	{
		if ((a_Chunk == nullptr) || !a_Chunk->IsValid())
		{
			return;
		}
	}

	std::vector<Vector3i> BlocksPotentiallyUnpowered;
	auto Data = (cIncrementalRedstoneSimulator::cIncrementalRedstoneSimulatorChunkData *)a_Chunk->GetRedstoneSimulatorData();

	Data->m_LinkedBlocks.erase(std::remove_if(Data->m_LinkedBlocks.begin(), Data->m_LinkedBlocks.end(), [&BlocksPotentiallyUnpowered, a_Chunk, a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ](const sLinkedPoweredBlocks & itr)
		{
			if (itr.a_MiddlePos.Equals(Vector3i(a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ)))
			{
				BlocksPotentiallyUnpowered.emplace_back(itr.a_BlockPos);
				a_Chunk->SetIsRedstoneDirty(true);
				return true;
			}
			return false;
		}
	), Data->m_LinkedBlocks.end());

	if (a_IsFirstCall && AreCoordsOnChunkBoundary(a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ))
	{
		// +- 2 to accomodate linked powered blocks
		SetInvalidMiddleBlock(a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ, a_Chunk->GetRelNeighborChunk(a_RelMiddleX - 2, a_RelMiddleZ), false);
		SetInvalidMiddleBlock(a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ, a_Chunk->GetRelNeighborChunk(a_RelMiddleX + 2, a_RelMiddleZ), false);
		SetInvalidMiddleBlock(a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ, a_Chunk->GetRelNeighborChunk(a_RelMiddleX, a_RelMiddleZ - 2), false);
		SetInvalidMiddleBlock(a_RelMiddleX, a_RelMiddleY, a_RelMiddleZ, a_Chunk->GetRelNeighborChunk(a_RelMiddleX, a_RelMiddleZ + 2), false);
	}

	for (const auto & itr : BlocksPotentiallyUnpowered)
	{
		if (!AreCoordsPowered(itr.x, itr.y, itr.z))
		{
			SetSourceUnpowered(itr.x, itr.y, itr.z, a_Chunk->GetRelNeighborChunk(itr.x, itr.z));
		}
	}
}





cIncrementalRedstoneSimulator::eRedstoneDirection cIncrementalRedstoneSimulator::GetWireDirection(int a_RelBlockX, int a_RelBlockY, int a_RelBlockZ)
{
	int Dir = REDSTONE_NONE;

	BLOCKTYPE NegX = 0;
	if (m_Chunk->UnboundedRelGetBlockType(a_RelBlockX - 1, a_RelBlockY, a_RelBlockZ, NegX))
	{
		if (IsPotentialSource(NegX))
		{
			Dir |= (REDSTONE_X_POS);
		}
	}

	BLOCKTYPE PosX = 0;
	if (m_Chunk->UnboundedRelGetBlockType(a_RelBlockX + 1, a_RelBlockY, a_RelBlockZ, PosX))
	{
		if (IsPotentialSource(PosX))
		{
			Dir |= (REDSTONE_X_NEG);
		}
	}

	BLOCKTYPE NegZ = 0;
	if (m_Chunk->UnboundedRelGetBlockType(a_RelBlockX, a_RelBlockY, a_RelBlockZ - 1, NegZ))
	{
		if (IsPotentialSource(NegZ))
		{
			if ((Dir & REDSTONE_X_POS) && !(Dir & REDSTONE_X_NEG))  // corner
			{
				Dir ^= REDSTONE_X_POS;
				Dir |= REDSTONE_X_NEG;
			}
			if ((Dir & REDSTONE_X_NEG) && !(Dir & REDSTONE_X_POS))  // corner
			{
				Dir ^= REDSTONE_X_NEG;
				Dir |= REDSTONE_X_POS;
			}
			Dir |= REDSTONE_Z_POS;
		}
	}

	BLOCKTYPE PosZ = 0;
	if (m_Chunk->UnboundedRelGetBlockType(a_RelBlockX, a_RelBlockY, a_RelBlockZ + 1, PosZ))
	{
		if (IsPotentialSource(PosZ))
		{
			if ((Dir & REDSTONE_X_POS) && !(Dir & REDSTONE_X_NEG))  // corner
			{
				Dir ^= REDSTONE_X_POS;
				Dir |= REDSTONE_X_NEG;
			}
			if ((Dir & REDSTONE_X_NEG) && !(Dir & REDSTONE_X_POS))  // corner
			{
				Dir ^= REDSTONE_X_NEG;
				Dir |= REDSTONE_X_POS;
			}
			Dir |= REDSTONE_Z_NEG;
		}
	}
	return (eRedstoneDirection)Dir;
}





bool cIncrementalRedstoneSimulator::IsLeverOn(NIBBLETYPE a_BlockMeta)
{
	// Extract the ON bit from metadata and return if true if it is set:
	return ((a_BlockMeta & 0x8) == 0x8);
}




