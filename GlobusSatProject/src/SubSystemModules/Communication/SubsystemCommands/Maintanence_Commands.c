#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "GlobalStandards.h"

#include <hal/Timing/Time.h>

#include <satellite-subsystems/IsisTRXVU.h>
#include <satellite-subsystems/IsisAntS.h>
#ifdef ISISEPS
	#include <satellite-subsystems/isis_eps_driver.h>
#endif
#ifdef GOMEPS
	#include <satellite-subsystems/GomEPS.h>
#endif


#include <hcc/api_fat.h>
#include <hal/Drivers/I2C.h>
#include <stdlib.h>
#include <string.h>
#include <hal/errors.h>
#include "TLM_management.h"
#include "SubSystemModules/Communication/TRXVU.h"
#include "SubSystemModules/Communication/AckHandler.h"
#include "SubSystemModules/Maintenance/Maintenance.h"
#include "Maintanence_Commands.h"

#define RESET_KEY 0xA6 // need to sed this key to the reset command otherwise reset will not happen

int CMD_GenericI2C(sat_packet_t *cmd) // TODO: why do we need this funcation?? it is using malloc
{
	if(cmd == NULL || cmd->data == NULL){
		return E_INPUT_POINTER_NULL;
	}
	int err = 0;
	unsigned char slaveAddr = 0;
	unsigned int size = 0;
	unsigned char *i2c_data = malloc(size);

	memcpy(&slaveAddr,cmd->data,sizeof(slaveAddr));
	memcpy(&size,cmd->data + sizeof(slaveAddr),sizeof(size));

	unsigned int offset = sizeof(slaveAddr) + sizeof(size);
	err = I2C_write((unsigned int)slaveAddr,cmd->data + offset, cmd->length);
	I2C_read((unsigned int)slaveAddr,i2c_data,size);

	return err;
}

int CMD_FRAM_ReadAndTransmitt(sat_packet_t *cmd) // TODO: why do we need this funcation?? it is using malloc
{
	if (cmd == NULL || cmd->data == NULL){
		return E_INPUT_POINTER_NULL;
	}
	int err = 0;
	unsigned int addr = 0;
	unsigned int size = 0;

	memcpy(&addr, cmd->data, sizeof(addr));
	memcpy(&size, cmd->data + sizeof(addr),sizeof(size));

	unsigned char *read_data = malloc(size);
	if(NULL == read_data){
		return E_MEM_ALLOC;
	}

	err = FRAM_read(read_data, addr, size);
	if (err != 0){
		return err;
	}

	TransmitDataAsSPL_Packet(cmd, read_data, size);
	free(read_data);
	return err;
}

int CMD_FRAM_WriteAndTransmitt(sat_packet_t *cmd) // TODO: why do we need this funcation??
{
	if (cmd == NULL || cmd->data == NULL){
		return E_INPUT_POINTER_NULL;
	}
	int err = 0;
	unsigned int addr = 0;
	unsigned int length = cmd->length;
	unsigned char *data = cmd->data;

	memcpy(&addr, cmd->data, sizeof(addr));

	err = FRAM_write(data + sizeof(addr), addr, length - sizeof(addr));
	if (err != 0){
		return err;
	}
	err = FRAM_read(data, addr, length - sizeof(addr));
	if (err != 0){
		return err;
	}
	TransmitDataAsSPL_Packet(cmd, data, length);
	return err;
}

int CMD_FRAM_Start(sat_packet_t *cmd)
{
	int err = 0;
	err = FRAM_start();
	TransmitDataAsSPL_Packet(cmd, (unsigned char*)&err, sizeof(err));
	return err;
}

int CMD_FRAM_Stop(sat_packet_t *cmd)
{
	(void)cmd;
	int err = 0;
	FRAM_stop();
	return err;
}

int CMD_FRAM_GetDeviceID(sat_packet_t *cmd)
{
	int err = 0;
	unsigned char id;
	FRAM_getDeviceID(&id);
	TransmitDataAsSPL_Packet(cmd, &id, sizeof(id));
	return err;
}

int CMD_UpdateSatTime(sat_packet_t *cmd)
{
	if (cmd == NULL || cmd->data == NULL){
		return E_INPUT_POINTER_NULL;
	}
	int err = 0;
	time_unix set_time = 0;
	memcpy(&set_time, cmd->data, sizeof(set_time));
	err = Time_setUnixEpoch(set_time);
	TransmitDataAsSPL_Packet(cmd, (unsigned char*)&set_time, sizeof(set_time));
	return err;
}

int CMD_GetSatTime(sat_packet_t *cmd)
{
	int err = 0;
	time_unix curr_time = 0;
	err = Time_getUnixEpoch(&curr_time);
	if (err != 0)
	{
		return err;
	}
	TransmitDataAsSPL_Packet(cmd, (unsigned char*)&curr_time, sizeof(curr_time));

	return err;
}

int CMD_GetSatUptime(sat_packet_t *cmd)
{
	int err = 0;
	time_unix uptime = 0;
	uptime = Time_getUptimeSeconds();
	TransmitDataAsSPL_Packet(cmd, (unsigned char*)&uptime, sizeof(uptime));
	return err;
}

int CMD_SoftTRXVU_ComponenetReset(sat_packet_t *cmd)
{
	if (cmd == NULL || cmd->data == NULL)
	{
		return E_INPUT_POINTER_NULL;
	}

	int err = 0;
	ISIStrxvuComponent component;
	memcpy(&component, cmd->data, sizeof(component));

	err = IsisTrxvu_componentSoftReset(ISIS_TRXVU_I2C_BUS_INDEX, component);
	return err;
}

int CMD_HardTRXVU_ComponenetReset(sat_packet_t *cmd)
{
	if (cmd == NULL || cmd->data == NULL)
	{
		return E_INPUT_POINTER_NULL;
	}

	int err = 0;
	ISIStrxvuComponent component;
	memcpy(&component, cmd->data, sizeof(component));

	err = IsisTrxvu_componentHardReset(ISIS_TRXVU_I2C_BUS_INDEX, component);
	return err;
}

int CMD_AntennaDeploy(sat_packet_t *cmd)
{
	(void)cmd;
	int err = 0;
	err = IsisAntS_setArmStatus(ISIS_TRXVU_I2C_BUS_INDEX , isisants_sideA, isisants_arm);
	if(err != E_NO_SS_ERR ){
		return err;
	}

	err = IsisAntS_setArmStatus(ISIS_TRXVU_I2C_BUS_INDEX , isisants_sideB, isisants_arm);
	if(err != E_NO_SS_ERR ){
		return err;
	}

	err = IsisAntS_autoDeployment(ISIS_TRXVU_I2C_BUS_INDEX, isisants_sideA,
			ANTENNA_DEPLOYMENT_TIMEOUT);
	if(err != E_NO_SS_ERR ){
		return err;
	}
	err = IsisAntS_autoDeployment(ISIS_TRXVU_I2C_BUS_INDEX, isisants_sideB,
				ANTENNA_DEPLOYMENT_TIMEOUT);
	return err;
}

int CMD_ResetComponent(reset_type_t rst_type)
{
	int err = 0;

	Boolean8bit reset_flag = TRUE_8BIT;

	switch (rst_type)
	{
	case reset_software:
		SendAnonymosAck(ACK_SOFT_RESET);
		FRAM_write(&reset_flag, RESET_CMD_FLAG_ADDR, RESET_CMD_FLAG_SIZE);
		vTaskDelay(10);
		restart();
		break;

	case reset_hardware:
		SendAnonymosAck(ACK_HARD_RESET);
		FRAM_write(&reset_flag, RESET_CMD_FLAG_ADDR, RESET_CMD_FLAG_SIZE);
		vTaskDelay(10);
		//TODO: ASK if we need this as we have the eps reset
		break;

	case reset_eps: // this is a HW reset of the iOBC
		SendAnonymosAck(ACK_EPS_RESET);
		FRAM_write(&reset_flag, RESET_CMD_FLAG_ADDR, RESET_CMD_FLAG_SIZE);
		vTaskDelay(10);
		isis_eps__reset__to_t cmd_t;
		isis_eps__reset__from_t cmd_f;
		logError(isis_eps__reset__tmtc(EPS_I2C_BUS_INDEX, &cmd_t, &cmd_f));
		break;

	case reset_trxvu_hard:
		SendAnonymosAck(ACK_TRXVU_HARD_RESET);
		logError(IsisTrxvu_hardReset(ISIS_TRXVU_I2C_BUS_INDEX));
		vTaskDelay(100);
		break;

	case reset_trxvu_soft:
		SendAnonymosAck(ACK_TRXVU_SOFT_RESET);
		logError(IsisTrxvu_softReset(ISIS_TRXVU_I2C_BUS_INDEX));
		vTaskDelay(100);
		break;

	case reset_filesystem:
		DeInitializeFS(); //TODO
		vTaskDelay(10);
		err = (unsigned int) InitializeFS(FALSE);
		vTaskDelay(10);
		SendAckPacket(ACK_FS_RESET, NULL, (unsigned char*) &err, sizeof(err));
		break;

	case reset_ant_SideA:
		logError(IsisAntS_reset(ISIS_TRXVU_I2C_BUS_INDEX, isisants_sideA));
		SendAckPacket(ACK_ANTS_RESET, NULL, (unsigned char*) &err, sizeof(err));
		break;

	case reset_ant_SideB:
		logError(IsisAntS_reset(ISIS_TRXVU_I2C_BUS_INDEX, isisants_sideB));
		SendAckPacket(ACK_ANTS_RESET, NULL, (unsigned char*) &err, sizeof(err));
		break;

	default:
		SendAnonymosAck(ACK_UNKNOWN_SUBTYPE);
		break;
	}
	vTaskDelay(10);
	return err;
}
