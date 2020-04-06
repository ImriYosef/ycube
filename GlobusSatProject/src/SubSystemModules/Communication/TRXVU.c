#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <hal/Timing/Time.h>
#include <hal/errors.h>

#include <satellite-subsystems/IsisTRXVU.h>
#include <satellite-subsystems/IsisAntS.h>

#include <stdlib.h>
#include <string.h>

#include "GlobalStandards.h"
#include "TRXVU.h"
#include "AckHandler.h"
#include "SubsystemCommands/TRXVU_Commands.h"
#include "SatCommandHandler.h"
#include "TLM_management.h"

#include "SubSystemModules/PowerManagment/EPS.h"
#include "SubSystemModules/Maintenance/Maintenance.h"
#include "SubSystemModules/Housekepping/TelemetryCollector.h"
#ifdef TESTING_TRXVU_FRAME_LENGTH
#include <hal/Utility/util.h>
#endif


time_unix 		g_mute_end_time = 0;				// time at which the mute will end
time_unix 		g_idle_end_time = 0;				// time at which the idel will end

xQueueHandle xDumpQueue = NULL;
xSemaphoreHandle xDumpLock = NULL;
xSemaphoreHandle xIsTransmitting = NULL; // mutex on transmission.
xTaskHandle xDumpHandle = NULL;

time_unix g_prev_beacon_time = 0;				// the time at which the previous beacon occured
time_unix g_beacon_interval_time = 0;			// seconds between each beacon



void InitTxModule()
{
	if(xIsTransmitting == NULL)
		vSemaphoreCreateBinary(xIsTransmitting);
}

int CMD_SetBeaconInterval(sat_packet_t *cmd)
{
	int err = 0;
	time_unix interval = 0;
	err =  FRAM_write((unsigned char*) &cmd->data,
			BEACON_INTERVAL_TIME_ADDR,
			BEACON_INTERVAL_TIME_SIZE);

	err += FRAM_read((unsigned char*) &interval,
			BEACON_INTERVAL_TIME_ADDR,
			BEACON_INTERVAL_TIME_SIZE);

	g_beacon_interval_time=interval;
	TransmitDataAsSPL_Packet(cmd, (unsigned char*) &interval,sizeof(interval));
	return err;
}



void InitBeaconParams()
{

	if (0	!= FRAM_read((unsigned char*) &g_beacon_interval_time,BEACON_INTERVAL_TIME_ADDR,BEACON_INTERVAL_TIME_SIZE)){
		g_beacon_interval_time = DEFAULT_BEACON_INTERVAL_TIME;
	}
}

void InitSemaphores()
{

	if(xDumpLock == NULL)
		vSemaphoreCreateBinary(xDumpLock);
	if(xDumpQueue == NULL)
		xDumpQueue = xQueueCreate(1, sizeof(Boolean));
}


int InitTrxvu() {
	ISIStrxvuI2CAddress myTRXVUAddress;
	ISIStrxvuFrameLengths myTRXVUFramesLenght;


	//Buffer definition
	myTRXVUFramesLenght.maxAX25frameLengthTX = SIZE_TXFRAME;//SIZE_TXFRAME;
	myTRXVUFramesLenght.maxAX25frameLengthRX = SIZE_RXFRAME;

	//I2C addresses defined
	myTRXVUAddress.addressVu_rc = I2C_TRXVU_RC_ADDR;
	myTRXVUAddress.addressVu_tc = I2C_TRXVU_TC_ADDR;


	//Bitrate definition
	ISIStrxvuBitrate myTRXVUBitrates;
	myTRXVUBitrates = trxvu_bitrate_9600;
	if (logError(IsisTrxvu_initialize(&myTRXVUAddress, &myTRXVUFramesLenght,&myTRXVUBitrates, 1))) return -1;

	vTaskDelay(100); //TODO why 100?? 100 what??

	if (logError(IsisTrxvu_tcSetAx25Bitrate(ISIS_TRXVU_I2C_BUS_INDEX,myTRXVUBitrates))) return -1;
	vTaskDelay(100);


	ISISantsI2Caddress myAntennaAddress;
	myAntennaAddress.addressSideA = ANTS_I2C_SIDE_A_ADDR;
	myAntennaAddress.addressSideB = ANTS_I2C_SIDE_B_ADDR;

	//Initialize the AntS system
	if (logError(IsisAntS_initialize(&myAntennaAddress, 1))) return -1;

	InitTxModule();
	InitBeaconParams();
	InitSemaphores();


	return 0;
}

void checkIdleFinish(){
	time_unix curr_tick_time = 0;
	Time_getUnixEpoch(&curr_tick_time);

	// check if it is time to turn off the idle...
	if (g_idle_end_time !=0 && g_idle_end_time < curr_tick_time){
		g_idle_end_time = 0;
		SetIdleState(trxvu_idle_state_off,0);
	}
}

int TRX_Logic() {
	int err = 0;
	int cmdFound=-1;
	// check if we have frames (data) waiting in the TRXVU
	int frameCount = GetNumberOfFramesInBuffer();
	sat_packet_t cmd = { 0 }; // holds the SPL command data

	if (frameCount > 0) {
		// we have data that came from grand station
		cmdFound = GetOnlineCommand(&cmd); //--> check - don't reset WDT if we got error getting the frame becuase we will never get a reset !
		ResetGroundCommWDT();
		err = SendAckPacket(ACK_RECEIVE_COMM, &cmd, NULL, 0);

	}

	if (cmdFound == command_found) {
		err = ActUponCommand(&cmd);
		//TODO: log error
		//TODO: send message to ground when a delayed command was not executed-> add to log
	}

	checkIdleFinish();
	BeaconLogic();

	if (logError(err)) return -1;

	return command_succsess;
}

int GetNumberOfFramesInBuffer() {
	unsigned short frameCounter = 0;
	if (logError(IsisTrxvu_rcGetFrameCount(0, &frameCounter))) return -1;

	return frameCounter;
}


int GetOnlineCommand(sat_packet_t *cmd)
{
	if (NULL == cmd) {
		return null_pointer_error;
	}
	int err = 0;

	unsigned short frameCount = 0;
	unsigned char receivedFrameData[MAX_COMMAND_DATA_LENGTH];

	if (logError(IsisTrxvu_rcGetFrameCount(0, &frameCount))) return -1;

	if (frameCount==0) {
		return no_command_found;
	}

	ISIStrxvuRxFrame rxFrameCmd = { 0, 0, 0,
			(unsigned char*) receivedFrameData }; // for getting raw data from Rx, nullify values

	if (logError(IsisTrxvu_rcGetCommandFrame(0, &rxFrameCmd))) return -1;
	// TODO log the RSSI from the frame
	if (logError(ParseDataToCommand(receivedFrameData,cmd))) return -1;


	return command_found;

}

// check the txSemaphore & low_voltage_flag
Boolean CheckTransmitionAllowed() {
	Boolean low_voltage_flag = TRUE;

	low_voltage_flag = EpsGetLowVoltageFlag();
	if (low_voltage_flag) return FALSE;

	// get current unix time
	time_unix curr_tick_time = 0;
	Time_getUnixEpoch(&curr_tick_time);

	if (curr_tick_time < g_mute_end_time) return FALSE;


	// check that we can take the tx Semaphore
	if(pdTRUE == xSemaphoreTake(xIsTransmitting,WAIT_TIME_SEM_TX)){
		xSemaphoreGive(xIsTransmitting);
		return TRUE;
	}
	return FALSE;

}

void FinishDump(dump_arguments_t *task_args,unsigned char *buffer, ack_subtype_t acktype,
		unsigned char *err, unsigned int size) {

	SendAckPacket(acktype, task_args->cmd, err, size);
	if (NULL != task_args) {
		free(task_args); // TODO: check why we need this?
	}
	if (NULL != xDumpLock) {
		xSemaphoreGive(xDumpLock);
	}
	if (xDumpHandle != NULL) {
		vTaskDelete(xDumpHandle);
	}
	if(NULL != buffer){
		free(buffer);
	}
}


void AbortDump()
{
	FinishDump(NULL,NULL,ACK_DUMP_ABORT,NULL,0);
}

void SendDumpAbortRequest() {
	if (eTaskGetState(xDumpHandle) == eDeleted) {
		return;
	}
	Boolean queue_msg = TRUE;
	int err = xQueueSend(xDumpQueue, &queue_msg, SECONDS_TO_TICKS(1));
	if (0 != err) {
		if (NULL != xDumpLock) {
			xSemaphoreGive(xDumpLock);
		}
		if (xDumpHandle != NULL) {
			vTaskDelete(xDumpHandle);
		}
	}
}

Boolean CheckDumpAbort() {
	return FALSE;
}




int BeaconLogic() {

	if(!CheckTransmitionAllowed()){
		return E_CANT_TRANSMIT;
	}
	// TODO get and set g_prev_beacon_time
	int err = 0;
	if (!CheckExecutionTime(g_prev_beacon_time, g_beacon_interval_time)) {
		return E_TOO_EARLY_4_BEACON;
	}

	WOD_Telemetry_t wod = { 0 };
	GetCurrentWODTelemetry(&wod);

	sat_packet_t cmd = { 0 };
	//TODO do we send beacon as SPL ???? there is no global standart ???

	if (logError(AssembleCommand((unsigned char*) &wod, sizeof(wod), trxvu_cmd_type,BEACON_SUBTYPE, 0xFFFFFFFF, &cmd))) return -1;

	// set the current time as the previous beacon time
	if (logError(Time_getUnixEpoch(&g_prev_beacon_time))) return -1;


	if (logError(TransmitSplPacket(&cmd, NULL))) return -1;
	// make sure we switch back to 9600 if we used 1200 in the beacon
	if (logError(IsisTrxvu_tcSetAx25Bitrate(ISIS_TRXVU_I2C_BUS_INDEX, trxvu_bitrate_9600))) return -1;

	printf("***************** beacon sent *****************\n");
	return 0;
}


int SetIdleState(ISIStrxvuIdleState state, time_unix duration){

	if (duration > MAX_IDLE_TIME) {
		logError(TRXVU_IDLE_TOO_LOMG);
		return TRXVU_IDLE_TOO_LOMG;
	}

	if (logError(IsisTrxvu_tcSetIdlestate(ISIS_TRXVU_I2C_BUS_INDEX, state))) return -1;

	if (state == trxvu_idle_state_on){
		// get current unix time
		time_unix curr_tick_time = 0;
		Time_getUnixEpoch(&curr_tick_time);

		// set mute end time
		g_idle_end_time = curr_tick_time + duration;
	}
	return 0;

}

int muteTRXVU(time_unix duration) {
	if (duration > MAX_MUTE_TIME) {
		logError(TRXVU_MUTE_TOO_LOMG);
		return -1;
	}
	// get current unix time
	time_unix curr_tick_time = 0;
	Time_getUnixEpoch(&curr_tick_time);

	// set mute end time
	g_mute_end_time = curr_tick_time + duration;
	return 0;

}

void UnMuteTRXVU() {
	g_mute_end_time = 0;

}


int TransmitDataAsSPL_Packet(sat_packet_t *cmd, unsigned char *data,
		unsigned int length) {
	int err = 0;
	sat_packet_t packet = { 0 };
	if (NULL != cmd) {
		err = AssembleCommand(data, length, cmd->cmd_type, cmd->cmd_subtype,
				cmd->ID, &packet);
	} else {
		err = AssembleCommand(data, length, 0xFF, 0xFF, 0xFFFFFFFF, &packet); //TODO: figure out what should be the 'FF'
	}
	if (err != 0) {
		return err;
	}
	err = TransmitSplPacket(&packet, NULL);
	return err;

}

int TransmitSplPacket(sat_packet_t *packet, int *avalFrames) {
	if (!CheckTransmitionAllowed()) {
		return E_CANT_TRANSMIT;
	}

	if ( packet == NULL) {
		return E_NOT_INITIALIZED;
	}

	int err = 0;
	unsigned int data_length = packet->length + sizeof(packet->length)
			+ sizeof(packet->cmd_subtype) + sizeof(packet->cmd_type)
			+ sizeof(packet->ID);

	if (xSemaphoreTake(xIsTransmitting,SECONDS_TO_TICKS(WAIT_TIME_SEM_TX)) != pdTRUE) { // TODO ask: isn't one tick too low ??
		return E_GET_SEMAPHORE_FAILED;
	}

	err = IsisTrxvu_tcSendAX25DefClSign(ISIS_TRXVU_I2C_BUS_INDEX,
			(unsigned char*) packet, data_length, (unsigned char*) avalFrames); // TOD ask: avalFrames null or zero ??

#ifdef TESTING
	printf("trxvu send ax25 error= %d",err);
	// display cmd packet values to screen
	printf(" id= %d",packet->ID);
	printf(" cmd type= %d",packet->cmd_type);
	printf(" cmd sub type= %d",packet->cmd_subtype);
	printf(" length= %d\n",packet->length);
	//printf(" data= %s\n",packet->data);
#endif

	xSemaphoreGive(xIsTransmitting);

	return err;

}

