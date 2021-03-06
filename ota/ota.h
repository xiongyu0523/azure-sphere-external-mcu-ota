﻿#ifndef OTA_H
#define OTA_H

#include "../parson.h"

enum ota_status_t
{
	otaDownloading = 0,
	otaInterrupted,
	otaApplying,
	otaApplied,
	otaError,
	otaStatusInvalid,
};

enum ota_error_t
{
	otaErrVerify = 0,
	otaErrHttp,
	otaErrTimeout,
	otaErrMcuDownload,
	otaErrIo,
	otaErrNone
};

int OtaInit(void);
void OtaHandler(const JSON_Object* extFwInfoProperties);
void OtaGetState(enum ota_status_t* p_status, enum ota_error_t* p_error);
uint32_t OtaGetVersion(void);

#endif