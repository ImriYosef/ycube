#include <string.h>
#include <stdlib.h>

#include <satellite-subsystems/IsisTRXVU.h>
#include <satellite-subsystems/IsisAntS.h>

#include "GlobalStandards.h"
#include "TRXVU_Commands.h"
#include "TLM_management.h"


extern xTaskHandle xDumpHandle;			 //task handle for dump task
extern xSemaphoreHandle xDumpLock; // this global lock is defined once in TRXVU.c

static dump_arguments_t dmp_pckt;

void DumpTask(void *args) {
	if (args == NULL) {
		FinishDump(NULL, NULL, ACK_DUMP_ABORT, NULL, 0);
		return;
	}


	// start the SD FS for this dump task
	logError(f_enterFS());

	//dump_arguments_t t = *((dump_arguments_t*)args);
	dump_arguments_t *task_args = (dump_arguments_t *) args;

	SendAckPacket(ACK_DUMP_START, task_args->cmd,
			NULL, 0);

	int numOfElementsSent = 0;
	// calculate how many days we were asked to dump (every day has 86400 seconds)
	int numberOfDays = (task_args->t_end - task_args->t_start)/86400;
	if (task_args->cmd->cmd_subtype == DUMP_DAYS){
		Time date;
		timeU2time(task_args->t_start,&date);
		numOfElementsSent = readTLMFiles(task_args->dump_type,date,numberOfDays,task_args->cmd->ID,task_args->resulotion);
	}else{ // DUMP_TIME_RANGE
		numOfElementsSent = readTLMFileTimeRange(task_args->dump_type,task_args->t_start,task_args->t_end,task_args->cmd->ID,task_args->resulotion);
	}

	FinishDump(task_args, NULL, ACK_DUMP_FINISHED, &numOfElementsSent, sizeof(numOfElementsSent));
	vTaskDelete(NULL); // kill the dump task

}


int CMD_StartDump(sat_packet_t *cmd)
{
	if (NULL == cmd) {
		return -1;
	}

	//dump_arguments_t dmp_pckt;
	//dump_arguments_t *dmp_pckt = malloc(sizeof(*dmp_pckt));
	unsigned int offset = 0;

	dmp_pckt.cmd = cmd;

	memcpy(&dmp_pckt.dump_type, cmd->data, sizeof(dmp_pckt.dump_type));
	offset += sizeof(dmp_pckt.dump_type);

	memcpy(&dmp_pckt.t_start, cmd->data + offset, sizeof(dmp_pckt.t_start));
	offset += sizeof(dmp_pckt.t_start);

	memcpy(&dmp_pckt.t_end, cmd->data + offset, sizeof(dmp_pckt.t_end));
	offset += sizeof(dmp_pckt.t_end);

	memcpy(&dmp_pckt.resulotion, cmd->data + offset, sizeof(dmp_pckt.resulotion));


	if (xSemaphoreTake(xDumpLock,SECONDS_TO_TICKS(WAIT_TIME_SEM_DUMP)) != pdTRUE) {
		return E_GET_SEMAPHORE_FAILED;
	}
	xTaskCreate(DumpTask, (const signed char* const )"DumpTask", 2000,
			&dmp_pckt, configMAX_PRIORITIES - 2, xDumpHandle);

	return 0;
}



int CMD_SendDumpAbortRequest(sat_packet_t *cmd)
{
	(void)cmd;
	SendDumpAbortRequest();
	return 0;
}

int CMD_ForceDumpAbort(sat_packet_t *cmd)
{
	(void)cmd;
	AbortDump();
	return 0;
}


int CMD_MuteTRXVU(sat_packet_t *cmd)
{
	int err = 0;
	time_unix mute_duaration = 0;
	memcpy(&mute_duaration,cmd->data,sizeof(mute_duaration));
	err = muteTRXVU(mute_duaration);
	SendAckPacket(MUTE_TRXVU,cmd,NULL,0); //trying to add the ack functions
	return err;
}

int CMD_SetIdleState(sat_packet_t *cmd)
{
	char state;
	memcpy(&state,cmd->data,sizeof(state));
	time_unix duaration = 0;
	if (state == trxvu_idle_state_on){
		memcpy(&duaration,cmd->data+sizeof(state),sizeof(duaration));
	}
	SetIdleState(state,duaration);
	return 0;
}


int CMD_UnMuteTRXVU(sat_packet_t *cmd)
{
	(void)cmd;
	UnMuteTRXVU();
	return 0;
}

int CMD_GetBaudRate(sat_packet_t *cmd)
{
	//ISIStrxvuBitrateStatus bitrate;
	ISIStrxvuTransmitterState trxvu_state;
	if (logError(IsisTrxvu_tcGetState(ISIS_TRXVU_I2C_BUS_INDEX, &trxvu_state))) return -1;

	int bitrate = trxvu_state.fields.transmitter_bitrate; //isn't bitrate a char?
	TransmitDataAsSPL_Packet(cmd, (unsigned char*) &bitrate, sizeof(bitrate));

	return 0;
}



int CMD_GetBeaconInterval(sat_packet_t *cmd)
{
	int err = 0;
	time_unix beacon_interval = 0;
	err = FRAM_read((unsigned char*) &beacon_interval,
			BEACON_INTERVAL_TIME_ADDR,
			BEACON_INTERVAL_TIME_SIZE);
	TransmitDataAsSPL_Packet(cmd, (unsigned char*) &beacon_interval,
			sizeof(beacon_interval));
	return err;
}


int CMD_SetBaudRate(sat_packet_t *cmd)
{
	// TODO do want to save the new bit rate in the FRAME so after restart (init_logic) we will continute to use the new rate??
	ISIStrxvuBitrateStatus bitrate;
	bitrate = (ISIStrxvuBitrateStatus) cmd->data[0];
	return IsisTrxvu_tcSetAx25Bitrate(ISIS_TRXVU_I2C_BUS_INDEX, bitrate);
}


int CMD_GetTxUptime(sat_packet_t *cmd)
{
	int err = 0;
	time_unix uptime = 0;
	err = IsisTrxvu_tcGetUptime(ISIS_TRXVU_I2C_BUS_INDEX, (unsigned int*)&uptime);
	TransmitDataAsSPL_Packet(cmd, (unsigned char*)&uptime, sizeof(uptime));

	return err;
}

int CMD_GetRxUptime(sat_packet_t *cmd)
{
	int err = 0;
	time_unix uptime = 0;
	err = IsisTrxvu_rcGetUptime(ISIS_TRXVU_I2C_BUS_INDEX,(unsigned int*) &uptime);
	TransmitDataAsSPL_Packet(cmd, (unsigned char*) &uptime, sizeof(uptime));

	return err;
}

int CMD_GetNumOfDelayedCommands(sat_packet_t *cmd)
{
	int err = 0;
	unsigned char temp = 0;
	temp = GetDelayedCommandBufferCount();
	TransmitDataAsSPL_Packet(cmd, (unsigned char*) &temp, sizeof(temp));

	return err;
}

int CMD_GetNumOfOnlineCommands(sat_packet_t *cmd)
{
	int err = 0;
	unsigned short int temp = 0;
	err = IsisTrxvu_rcGetFrameCount(ISIS_TRXVU_I2C_BUS_INDEX, &temp);
	TransmitDataAsSPL_Packet(cmd, (unsigned char*) &temp, sizeof(temp));

	return err;
}

int CMD_DeleteDelyedCmdByID(sat_packet_t *cmd)
{
	int err = 0;
	unsigned int index = 0;
	memcpy(&index,cmd->data,sizeof(index));
	err = DeleteDelayedCommandByIndex(index);
	return err;
}

int CMD_DeleteAllDelyedBuffer(sat_packet_t *cmd)
{
	(void)cmd;
	int err = 0;
	err = DeleteDelayedBuffer();
	return err;
}

int CMD_AntSetArmStatus(sat_packet_t *cmd)
{
	if (cmd == NULL || cmd->data == NULL) {
		return E_INPUT_POINTER_NULL;
	}
	int err = 0;
	ISISantsSide ant_side = cmd->data[0];

	ISISantsArmStatus status = cmd->data[1];
	err = IsisAntS_setArmStatus(ISIS_TRXVU_I2C_BUS_INDEX, ant_side, status);

	return err;
}

int CMD_AntGetArmStatus(sat_packet_t *cmd)
{
	int err = 0;
	ISISantsStatus status;
	ISISantsSide ant_side;
	memcpy(&ant_side, cmd->data, sizeof(ant_side)); //why memcpy? its a char, only one bit

	err = IsisAntS_getStatusData(ISIS_TRXVU_I2C_BUS_INDEX, ant_side, &status);
	TransmitDataAsSPL_Packet(cmd, (unsigned char*) &status, sizeof(status));

	return err;
}

int CMD_AntGetUptime(sat_packet_t *cmd)
{
	int err = 0;
	time_unix uptime = 0;
	ISISantsSide ant_side;
	memcpy(&ant_side, cmd->data, sizeof(ant_side));
	err = IsisAntS_getUptime(ISIS_TRXVU_I2C_BUS_INDEX, ant_side,(unsigned int*) &uptime);
	TransmitDataAsSPL_Packet(cmd, (unsigned char*) &uptime, sizeof(uptime));
	return err;
}

int CMD_AntCancelDeployment(sat_packet_t *cmd)
{
	int err = 0;
	ISISantsSide ant_side;
	memcpy(&ant_side, cmd->data, sizeof(ant_side));
	err = IsisAntS_cancelDeployment(ISIS_TRXVU_I2C_BUS_INDEX, ant_side);
	return err;
}



