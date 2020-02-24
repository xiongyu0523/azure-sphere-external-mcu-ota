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
#include "../littlefs_w25q128.h"
#include "../littlefs/lfs.h"

#include <curl/curl.h>
#include <curl/easy.h>

#include "extmcu_hal.h"
#include "ota.h"

#define MAX_REQUEST 3

static void OtaSetState(enum ota_status_t status, enum ota_error_t error);
static void OtaSetVersion(uint32_t version);

struct ota_request_t {
    uint32_t version;
    uint32_t size;
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
    uint32_t ota_version;
    int local_record_fd;
    pthread_t ota_thread;
    struct ota_queue_t ota_queue;
    lfs_t lfs
};

static struct ota_context_t* pOtaContext = NULL;

uint8_t dummy[1024 * 100];
uint32_t position = 0;

void __OtaEventDequeue(struct ota_request_t* req)
{
    (void)sem_wait(&pOtaContext->ota_queue.semaphr);

    (void)pthread_mutex_lock(&pOtaContext->ota_queue.lock);
    *req = pOtaContext->ota_queue.requests[pOtaContext->ota_queue.rpos++];
    if (pOtaContext->ota_queue.rpos >= MAX_REQUEST) {
        pOtaContext->ota_queue.rpos = 0;
    }
    (void)pthread_mutex_unlock(&pOtaContext->ota_queue.lock);
}

void __OtaEventEnqueue(struct ota_request_t *req)
{
    (void)pthread_mutex_lock(&pOtaContext->ota_queue.lock);
    pOtaContext->ota_queue.requests[pOtaContext->ota_queue.wpos++] = *req;
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

static bool __image_verify(lfs_file_t *p_file, const char *p_target_sha256_str)
{
    uint8_t hashValue[SHA256_BYTES];
    sha256_context ctx;
    char hashString[SHA256_BYTES * 2 + sizeof('\0')];
    char buffer[512];
    lfs_ssize_t nb;

    // to make it be a string
    hashString[SHA256_BYTES * 2] = '\0';

    lfs_file_seek(&pOtaContext->lfs, p_file, 0, LFS_SEEK_SET);
    sha256_init(&ctx);

    do {
        nb = lfs_file_read(&pOtaContext->lfs, p_file, buffer, 512);
        if (nb > 0) {
            sha256_hash(&ctx, &buffer[0], nb);
        } else if (nb == 0) {
            break;
        } else {
            Log_Debug("ERROR: IO Error during image verify\n");
            return false;
        }
    } while (true);

    sha256_done(&ctx, &hashValue[0]);

    for (uint32_t i = 0; i < SHA256_BYTES; i++) {
        sprintf(&hashString[i * 2], "%02X", hashValue[i]);
    }

    if (strcmp(hashString, p_target_sha256_str) == 0) {
        Log_Debug("INFO: Image verification pass!\n");
        return true;
    } else {
        Log_Debug("WARNING: Image verification fail, calculated sha256 = %s\n", hashString);
        return false;
    }
}

static void LogCurlError(const char* message, int curlErrCode)
{
    Log_Debug(message);
    Log_Debug(" (curl err=%d, '%s')\n", curlErrCode, curl_easy_strerror(curlErrCode));
}

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    lfs_file_t* p_file = userdata;

    if (lfs_file_write(&pOtaContext->lfs, p_file, ptr, nmemb) != nmemb) {
        Log_Debug("ERROR: less number of bytes write to file\n");
        return 0;
    } else {
        return nmemb;
    }
}

static int dl_progress(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    Log_Debug("%d in %d bytes transfered\n", (int)dlnow, (int)dltotal);
    return 0;
}

static void* ota_thread(void* arg) 
{
    uint32_t local_version;
    struct ota_request_t req;

    uint32_t resume_offset;
    bool need_download;
    bool has_partial_image;
    bool finish_download;
    lfs_file_t ota_binary_file;

    while (1) {

        __OtaEventDequeue(&req);
        Log_Debug("Checking OTA, server version is %d\n", req.version);
        Log_Debug("URL = %s\n", req.p_url);
        Log_Debug("SAS = %s\n", req.p_sas);
        Log_Debug("SHA256 = %s\n", req.p_sha256);

        if (lfs_file_open(&pOtaContext->lfs, &ota_binary_file, "ota.bin", LFS_O_RDWR | LFS_O_CREAT) != LFS_ERR_OK) {
            Log_Debug("ERROR: Unable to open ota.bin file\n");
            OtaSetState(otaError, otaErrIo);
            free(req.p_url);
            free(req.p_sas);
            free(req.p_sha256);
            continue;
        }

        resume_offset = 0;
        need_download = true;
        has_partial_image = false;
        finish_download = false;

        local_version = __get_local_record(&has_partial_image);
        
        // if the record file is {"Downloading":x}, it means there is a partial received image on file system
        if (has_partial_image) {

            // when x is newer version than server push, we do not roll back. or we can accept roll back depends real policy.
            if (local_version > req.version) {
                need_download = false;
            // when x is euqal to server version, we will try to resume from the last break point.
            } else if (local_version == req.version) {
                lfs_soff_t size = lfs_file_seek(&pOtaContext->lfs, &ota_binary_file, 0, LFS_SEEK_END);
                if (size >= 0) {
                    if (size < req.size) {
                        resume_offset = size;
                    } else if (size == req.size) {
                        need_download = false;
                        finish_download = true;
                    } else {
                        need_download = false;
                        Log_Debug("ERROR: Incorrect file size\n");
                    }
                } else {
                    resume_offset = 0;
                }
            }
        } else {
            // if the record file is {"Completed":x}, it means there is a previous completed image on file system
            if (local_version >= req.version) {
                // when x is a equal or newer version than on server, do not start. (also depends on roll back policy)
                need_download = false;
            }
        }

        if (need_download) {

            Log_Debug("Starting download from offset %d...\n", resume_offset);

            // For a refresh download, clean ota.bin file and change local record to {"Downloading":y}
            if (resume_offset == 0) {
                lfs_file_truncate(&pOtaContext->lfs, &ota_binary_file, 0);
                __update_local_record(req.version, false);
            }

            OtaSetState(otaDownloading, otaErrNone);

            sha256_context ctx;
            CURL* curlHandle = NULL;
            CURLcode res = CURLE_OK;
            struct curl_slist* list = NULL;

            (void)curl_global_init(CURL_GLOBAL_ALL);
            curlHandle = curl_easy_init();

            (void)curl_easy_setopt(curlHandle, CURLOPT_CAINFO, Storage_GetAbsolutePathInImagePackage("certs/root.pem"));
            char* sasurl = calloc(strlen(req.p_url) + sizeof('?') + strlen(req.p_sas) + sizeof('\0'), sizeof(char));
            (void)strcat(strcat(strcat(sasurl, req.p_url), "?"), req.p_sas);
            (void)curl_easy_setopt(curlHandle, CURLOPT_URL, sasurl);
            // specify Azure Blob REST API version, for version order than 2011-08-18 do not accept 'Range: bytes=start-' header
            list = curl_slist_append(list, "x-ms-version:2019-02-02");
            (void)curl_easy_setopt(curlHandle, CURLOPT_HTTPHEADER, list);
            (void)curl_easy_setopt(curlHandle, CURLOPT_HTTPGET, 1);
            (void)curl_easy_setopt(curlHandle, CURLOPT_RESUME_FROM, resume_offset);
            (void)curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, write_callback);
            (void)curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &ota_binary_file);
            (void)curl_easy_setopt(curlHandle, CURLOPT_FAILONERROR, 1);
            // abort if speed is below 10bytes/seconds for 30 seconds
            (void)curl_easy_setopt(curlHandle, CURLOPT_LOW_SPEED_TIME,  30);
            (void)curl_easy_setopt(curlHandle, CURLOPT_LOW_SPEED_LIMIT, 10);
            // Debug Options
            (void)curl_easy_setopt(curlHandle, CURLOPT_PROGRESSFUNCTION, dl_progress);
            (void)curl_easy_setopt(curlHandle, CURLOPT_NOPROGRESS, 0);
            (void)curl_easy_setopt(curlHandle, CURLOPT_VERBOSE, 1L);

            res = curl_easy_perform(curlHandle);
            if (res == CURLE_OK) {
                finish_download = true;
                Log_Debug("INFO: Download Finished, file size = %d\n", lfs_file_size(&pOtaContext->lfs, &ota_binary_file));
            } else {
                if (res == CURLE_OPERATION_TIMEDOUT) {
                    OtaSetState(otaInterrupted, otaErrTimeout);
                } else if (res == CURLE_HTTP_RETURNED_ERROR) {
                    OtaSetState(otaInterrupted, otaErrHttp);
                } else if (res == CURLE_WRITE_ERROR) {
                    OtaSetState(otaError, otaErrIo);
                }

                Log_Debug("INFO: Download interrupted, ret code = %d\n", res);
            }

            free(sasurl);
            curl_easy_cleanup(curlHandle);
            curl_global_cleanup();
        } 

        // A completed file is downloaded or has been download (if a powerfail happens after download and before verify pass)
        if (finish_download) {

            if (__image_verify(&ota_binary_file, req.p_sha256)) {
                __update_local_record(req.version, true);
            } else {
                // empty the file to make sure retry from start 
                (void)lfs_file_truncate(&pOtaContext->lfs, &ota_binary_file, 0);
                OtaSetState(otaError, otaErrVerify);
            }
        }
           
        // read again since a good ota will update local record
        local_version = __get_local_record(&has_partial_image);
        if ((!has_partial_image) && (ExtMCU_GetVersion() < local_version)) {

            OtaSetState(otaApplying, otaErrNone);

            if (ExtMCU_Download()) {
                OtaSetVersion(local_version);
                OtaSetState(otaApplied, otaErrNone);
            } else {
                OtaSetState(otaError, otaErrMcuDownload);
            }
        }

        lfs_file_close(&pOtaContext->lfs, &ota_binary_file);
        free(req.p_url);
        free(req.p_sas);
        free(req.p_sha256);
    }
}

void OtaHandler(const JSON_Object* extFwInfoProperties)
{
    struct ota_request_t req;

    if (pOtaContext->is_inited) {

        req.version = (uint32_t)json_object_get_number(extFwInfoProperties, "version");
        req.size = (uint32_t)json_object_get_number(extFwInfoProperties, "size");
        req.p_url = strdup(json_object_get_string(extFwInfoProperties, "url"));
        req.p_sas = strdup(json_object_get_string(extFwInfoProperties, "sas"));
        req.p_sha256 = strdup(json_object_get_string(extFwInfoProperties, "sha256"));

        if ((req.version > 0) && (req.size > 0) && (req.p_url != NULL) && (req.p_sas != NULL) && (req.p_sha256 != NULL)) {
            __OtaEventEnqueue(&req);
        } else {
            // free a NULL has no side effect..
            free(req.p_url);
            free(req.p_sas);
            free(req.p_sha256);
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

    w25q128_init();
    if (lfs_mount(&pOtaContext->lfs, &g_w25q128_littlefs_config) != LFS_ERR_OK) {
        Log_Debug("INFO: LFS Mount fail, try to format and re-mount\n");
        lfs_format(&pOtaContext->lfs, &g_w25q128_littlefs_config);
        lfs_mount(&pOtaContext->lfs, &g_w25q128_littlefs_config);
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
errExitLabel_1:
    free(pOtaContext);
    pOtaContext = NULL;
errExitLabel_0:
    return -1;
}
 
void OtaDeinit() {
    ;
}

static void OtaSetState(enum ota_status_t status, enum ota_error_t error) 
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

static void OtaSetVersion(uint32_t version)
{
    pOtaContext->ota_version = version;
}

uint32_t OtaGetVersion(void)
{
    return pOtaContext->ota_version;
}