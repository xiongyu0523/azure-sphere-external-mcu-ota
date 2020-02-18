/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <applibs/log.h>
#include <applibs/storage.h>

#include "../sha256/mark2/sha256.h"

#include <curl/curl.h>
#include <curl/easy.h>

#include "extmcu_hal.h"
#include "ota.h"

#define MAX_REQUEST 3

struct ota_request_t {
    uint32_t version;
    char *p_url;
    char *p_sas;
    char *p_sha256;
};

struct ota_queue_t {
    sem_t semaphr;
    pthread_mutex_t lock;
    struct ota_request_t requests[MAX_REQUEST];
    uint32_t wpos;
    uint32_t rpos;
};

struct ota_state_t {
    enum ota_status_t status;
    enum ota_error_t error;
    pthread_mutex_t lock;
};

struct ota_context_t {
    bool is_inited;
    struct ota_state_t ota_state;
    int local_record_fd;
    pthread_t ota_thread;
    struct ota_queue_t ota_queue;
};

static struct ota_context_t* pOtaContext = NULL;

uint8_t dummy[1024 * 100];
uint32_t position = 0;

void __OtaEventDequeue(uint32_t* ver, char** url, char **sas, char** sha256)
{
    (void)sem_wait(&pOtaContext->ota_queue.semaphr);

    (void)pthread_mutex_lock(&pOtaContext->ota_queue.lock);
    *ver = pOtaContext->ota_queue.requests[pOtaContext->ota_queue.rpos].version;
    *url = pOtaContext->ota_queue.requests[pOtaContext->ota_queue.rpos].p_url;
    *sas = pOtaContext->ota_queue.requests[pOtaContext->ota_queue.rpos].p_sas;
    *sha256 = pOtaContext->ota_queue.requests[pOtaContext->ota_queue.rpos].p_sha256;
    pOtaContext->ota_queue.rpos++;
    if (pOtaContext->ota_queue.rpos >= MAX_REQUEST) {
        pOtaContext->ota_queue.rpos = 0;
    }
    (void)pthread_mutex_unlock(&pOtaContext->ota_queue.lock);
}

void __OtaEventEnqueue(uint32_t ver, char* url, char *sas, char* sha256)
{
    (void)pthread_mutex_lock(&pOtaContext->ota_queue.lock);
    pOtaContext->ota_queue.requests[pOtaContext->ota_queue.wpos].version = ver;
    pOtaContext->ota_queue.requests[pOtaContext->ota_queue.wpos].p_url = url;
    pOtaContext->ota_queue.requests[pOtaContext->ota_queue.wpos].p_sas = sas;
    pOtaContext->ota_queue.requests[pOtaContext->ota_queue.wpos].p_sha256 = sha256;
    pOtaContext->ota_queue.wpos++;
    if (pOtaContext->ota_queue.wpos >= MAX_REQUEST) {
        pOtaContext->ota_queue.wpos = 0;
    }
    (void)pthread_mutex_unlock(&pOtaContext->ota_queue.lock);

    (void)sem_post(&pOtaContext->ota_queue.semaphr);
}

static void __update_local_record(uint32_t version, bool done)
{
#define MAX_RECORD_LEN 50
    char temp_buffer[MAX_RECORD_LEN];
    ssize_t len;

    lseek(pOtaContext->local_record_fd, 0, SEEK_SET);

    if (done) {
        snprintf(temp_buffer, MAX_RECORD_LEN, "{\"Completed\":%d}", version);
    } else {
        snprintf(temp_buffer, MAX_RECORD_LEN, "{\"Downloading\":%d}", version);
    }

    len = write(pOtaContext->local_record_fd, temp_buffer, strlen(temp_buffer) + 1);
    Log_Debug("Successfully write %d bytes to file\n", len);
}

static uint32_t __get_local_record(bool *has_partial_image)
{
    uint32_t version = 0;
    long total = 0;
    uint8_t *p_file_buffer = NULL;

    *has_partial_image = false;

    total = lseek(pOtaContext->local_record_fd, 0, SEEK_END);
    lseek(pOtaContext->local_record_fd, 0, SEEK_SET);

    if (total > 0) {

        p_file_buffer = malloc(total);
        if (p_file_buffer == NULL) {
            Log_Debug("ERROR: malloc fail.\n");
            goto cleanup;
        }

        ssize_t rd = read(pOtaContext->local_record_fd, p_file_buffer, total);
        if (rd < 0) {
            Log_Debug("ERROR: file read: %s (%d).\n", strerror(errno), errno);
            goto cleanup;
        }

        Log_Debug("Local record = %s\n", p_file_buffer);

        JSON_Value* root = json_parse_string(p_file_buffer);
        if (root == NULL) {
            Log_Debug("ERROR: Cannot parse the string as JSON content.\n");
            goto cleanup;
        }

        JSON_Object* rootObject = json_value_get_object(root);
        version = (uint32_t)json_object_get_number(rootObject, "Downloading");
        // there is no key "Downloading"
        if (version == 0) {
            version = (uint32_t)json_object_get_number(rootObject, "Completed");
            if (version == 0) {
                Log_Debug("ERROR: Do not find either 'Downloading' or 'Completed' key.\n");
            }
        }
        else {
            *has_partial_image = true;
        }
    }

cleanup:
    free(p_file_buffer);
    return version;
}

static void LogCurlError(const char* message, int curlErrCode)
{
    Log_Debug(message);
    Log_Debug(" (curl err=%d, '%s')\n", curlErrCode, curl_easy_strerror(curlErrCode));
}

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    // size is always 1, nmemb can varies, return a value != nmemb will terminate the transfer

    if (position == 0) {
        sha256_init((sha256_context*)userdata);
    }

    //memcpy(&dummy[position], ptr, nmemb);

    sha256_hash((sha256_context*)userdata, ptr, nmemb);

    position += nmemb;

    return nmemb;
}

static int dl_progress(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    if (dlnow && dltotal) {
        Log_Debug("dl:%3.0f%%\r", 100 * dlnow / dltotal);
    }
    return 0;
}


static void* ota_thread(void* arg) 
{
    uint32_t local_version;
    uint32_t server_version;
    char    *image_url;
    char    *image_sha256;
    char    *sas_token;

    uint32_t resume_offset;
    bool need_download;
    bool has_partial_image;

    while (1) {

        __OtaEventDequeue(&server_version, &image_url, &sas_token, &image_sha256);

        Log_Debug("Remote version = %d\n", server_version);

        resume_offset = 0;
        need_download = true;
        has_partial_image = false;

        local_version = __get_local_record(&has_partial_image);
        if (has_partial_image) {
            if (local_version > server_version) {
                need_download = false;
            } else if (local_version == server_version) {
                resume_offset = position;
                Log_Debug("Resuming download from %d\n", resume_offset);
            }
        } else {
            if (local_version >= server_version) {
                need_download = false;
            }
        }

        if (need_download) {

            Log_Debug("Starting download...\n");

            sha256_context ctx;
            CURL* curlHandle = NULL;
            CURLcode res = CURLE_OK;
            struct curl_slist* list = NULL;

            (void)curl_global_init(CURL_GLOBAL_ALL);
            curlHandle = curl_easy_init();

            (void)curl_easy_setopt(curlHandle, CURLOPT_CAINFO, Storage_GetAbsolutePathInImagePackage("certs/root.pem"));
            char* sasurl = calloc(strlen(image_url) + strlen(sas_token) + sizeof('\0'), sizeof(char));
            (void)strcat(strcat(sasurl, image_url), sas_token);
            (void)curl_easy_setopt(curlHandle, CURLOPT_URL, sasurl);
            // specify Azure Blob REST API version, for version order than 2011-08-18 do not accept 'Range: bytes=start-' header
            list = curl_slist_append(list, "x-ms-version:2019-02-02");
            (void)curl_easy_setopt(curlHandle, CURLOPT_HTTPHEADER, list);
            (void)curl_easy_setopt(curlHandle, CURLOPT_HTTPGET, 1);
            (void)curl_easy_setopt(curlHandle, CURLOPT_RESUME_FROM, resume_offset);
            (void)curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, write_callback);
            (void)curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &ctx);
            (void)curl_easy_setopt(curlHandle, CURLOPT_FAILONERROR, 1);
            // abort if speed is below 10bytes/seconds for 30 seconds
            (void)curl_easy_setopt(curlHandle, CURLOPT_LOW_SPEED_TIME,  30);
            (void)curl_easy_setopt(curlHandle, CURLOPT_LOW_SPEED_LIMIT, 10);
            // Debug Options
            (void)curl_easy_setopt(curlHandle, CURLOPT_PROGRESSFUNCTION, dl_progress);
            (void)curl_easy_setopt(curlHandle, CURLOPT_NOPROGRESS, 0);
            (void)curl_easy_setopt(curlHandle, CURLOPT_VERBOSE, 1L);

            // Do not change local recoard for resume downloading..
            if (resume_offset == 0) {
                __update_local_record(server_version, false);
            }

            OtaSetState(otaDownloading, otaErrNone);

            res = curl_easy_perform(curlHandle);
            if (res == CURLE_OK) {

                position = 0;

                uint8_t hashValue[SHA256_BYTES];
                char    hashString[SHA256_BYTES * 2 + sizeof('\0')];
                hashString[SHA256_BYTES * 2] = '\0';

                sha256_done(&ctx, &hashValue[0]);
                for (uint32_t i = 0; i < SHA256_BYTES; i++) {
                    sprintf(&hashString[i * 2], "%02X", hashValue[i]);
                }

                if (strcmp(hashString, image_sha256) == 0) {
                    Log_Debug("INFO: Image verification Pass!\n");

                    // update local record after sha256 verification
                    __update_local_record(server_version, true);

                } else {
                    OtaSetState(otaError, otaErrVerify);
                    Log_Debug("WARNING: Image verification fail\n");
                }
            } else {

                if (res == CURLE_OPERATION_TIMEDOUT) {
                    OtaSetState(otaInterrupted, otaErrTimeout);
                } else if (res == CURLE_HTTP_RETURNED_ERROR) {
                    OtaSetState(otaInterrupted, otaErrHttp);
                } 

                Log_Debug("INFO: Download interrupted, %d bytes has transfered\n", position);
            }

            free(sasurl);
            curl_easy_cleanup(curlHandle);
            curl_global_cleanup();
        } 
           
        // read again since ota will update local record
        local_version = __get_local_record(&has_partial_image);
        if ((!has_partial_image) && (ExtMCU_GetVersion() < local_version)) {

            OtaSetState(otaApplying, otaErrNone);

            if (ExtMCU_Download()) {
                OtaSetState(otaApplied, otaErrNone);
            } else {
                OtaSetState(otaError, otaErrMcuDownload);
            }
        }

        free(image_url);
        free(image_sha256);
    }
}

void OtaHandler(const JSON_Object* extFwInfoProperties)
{
    if (pOtaContext->is_inited) {

        uint32_t server_version = (uint32_t)json_object_get_number(extFwInfoProperties, "version");
        char *p_url = json_object_get_string(extFwInfoProperties, "url");
        char* p_sas = json_object_get_string(extFwInfoProperties, "sas");
        char *p_sha256 = json_object_get_string(extFwInfoProperties, "sha256");

        if ((server_version > 0) && (p_url != NULL) && (p_sas != NULL) && (p_sha256 != NULL)) {
            
            //__update_local_record(1, true);
            __OtaEventEnqueue(server_version, strdup(p_url), strdup(p_sas), strdup(p_sha256));
        }
    }
}

int OtaInit(void) 
{
    int rt = -1;

    if (pOtaContext == NULL) {
        pOtaContext = malloc(sizeof(struct ota_context_t));
        if (pOtaContext == NULL) {
            Log_Debug("ERROR: malloc fail\n");
            goto errExitLabel_0;
        }
    }

    memset(pOtaContext, 0, sizeof(struct ota_context_t));

    pOtaContext->local_record_fd = Storage_OpenMutableFile();
    if (pOtaContext->local_record_fd < 0) {
        Log_Debug("ERROR: Could not open mutable file: %s (%d)\n", strerror(errno), errno);
        goto errExitLabel_1;
    }

    rt = pthread_create(&pOtaContext->ota_thread, NULL, ota_thread, NULL);
    if (rt != 0) {
        Log_Debug("ERROR: Can not create a thread: %d\n", rt);
        goto errExitLabel_2;
    }

    rt = sem_init(&pOtaContext->ota_queue.semaphr, 0, 0);
    if (rt < 0) {
        Log_Debug("ERROR: Could not init semaphore: %s (%d)\n", strerror(errno), errno);
        goto errExitLabel_3;
    }

    rt = pthread_mutex_init(&pOtaContext->ota_queue.lock, NULL);
    if (rt < 0) {
        Log_Debug("ERROR: Could not init ota_queue mutex: %s (%d)\n", strerror(errno), errno);
        goto errExitLabel_4;
    }

    rt = pthread_mutex_init(&pOtaContext->ota_state.lock, NULL);
    if (rt < 0) {
        Log_Debug("ERROR: Could not init ota_state mutex: %s (%d)\n", strerror(errno), errno);
        goto errExitLabel_5;
    }

    pOtaContext->ota_queue.wpos = 0;
    pOtaContext->ota_queue.rpos = 0;
    pOtaContext->ota_state.status = otaStatusInvalid;
    pOtaContext->ota_state.error = otaErrNone;
    pOtaContext->is_inited = true;

    return 0;
errExitLabel_5:
    pthread_mutex_destroy(&pOtaContext->ota_queue.lock);
errExitLabel_4:
    sem_destroy(&pOtaContext->ota_queue.semaphr);
errExitLabel_3:
    // remove a thread;
errExitLabel_2:
    close(pOtaContext->local_record_fd);
    pOtaContext->local_record_fd = -1;
errExitLabel_1:
    free(pOtaContext);
errExitLabel_0:
    return -1;
}
 
void OtaDeinit() {
    ;
}

void OtaSetState(enum ota_status_t status, enum ota_error_t error) 
{
    (void)pthread_mutex_lock(&pOtaContext->ota_state.lock);
    pOtaContext->ota_state.status = status;
    pOtaContext->ota_state.error = error;
    (void)pthread_mutex_unlock(&pOtaContext->ota_state.lock);
}

void OtaGetState(enum ota_status_t *p_status, enum ota_error_t *p_error)
{
    (void)pthread_mutex_lock(&pOtaContext->ota_state.lock);
    *p_status = pOtaContext->ota_state.status;
    *p_error = pOtaContext->ota_state.error;
    (void)pthread_mutex_unlock(&pOtaContext->ota_state.lock);
}
