/**
 * @file ARSTREAMING_Reader_TestBench.c
 * @brief Testbench for the ARSTREAMING_Reader submodule
 * @date 03/25/2013
 * @author nicolas.brulez@parrot.com
 */

/*
 * System Headers
 */

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>

/*
 * ARSDK Headers
 */

#include <libARSAL/ARSAL_Print.h>
#include <libARSAL/ARSAL_Sem.h>
#include <libARStreaming/ARSTREAMING_Reader.h>

/*
 * Macros
 */

#define ACK_BUFFER_ID (13)
#define DATA_BUFFER_ID (125)

#define SENDING_PORT (43210)
#define READING_PORT (54321)

#define FRAME_MIN_SIZE (2000)
#define FRAME_MAX_SIZE (40000)

#define NB_BUFFERS (3)

#define __TAG__ "ARSTREAMING_Reader_TB"

#define READER_PING_DELAY (0) // Use default value

#ifndef __IP
#define __IP "127.0.0.1"
#endif

#define NB_FRAMES_FOR_AVERAGE (15)

/*
 * Types
 */

/*
 * Globals
 */


static struct timeval lastRecv = {0};
static int lastDt [NB_FRAMES_FOR_AVERAGE] = {0};
static int currentIndexInDt = 0;

float ARSTREAMING_Reader_PercentOk = 100.f;
static int nbRead = 0;
static int nbSkipped = 0;
static int nbSkippedSinceLast = 0;

static int running;
ARSAL_Sem_t closeSem;
static ARNETWORK_Manager_t *g_Manager = NULL;
static ARSTREAMING_Reader_t *g_Reader = NULL;

static int currentBufferIndex = 0;
static uint8_t *multiBuffer[NB_BUFFERS];
static uint32_t multiBufferSize[NB_BUFFERS];
static int multiBufferIsFree[NB_BUFFERS];

static char *appName;

static FILE *outFile;

/*
 * Internal functions declarations
 */

/**
 * @brief Print the parameters of the application
 */
void ARSTREAMING_ReaderTb_printUsage ();

/**
 * @brief Initializes the multi buffers of the testbench
 * @param initialSize Initial size of the buffers
 */
void ARSTREAMING_ReaderTb_initMultiBuffers (int initialSize);

/**
 * @brief Realloc a buffer to a new size
 * @param id The index of the buffer to reallocate
 * @param newSize The new size of the buffer
 */
void reallocBuffer (int id, int newSize);

/**
 * @see ARSTREAMING_Reader.h
 */
uint8_t* ARSTREAMING_ReaderTb_FrameCompleteCallback (eARSTREAMING_READER_CAUSE cause, uint8_t *framePointer, uint32_t frameSize, int numberOfSkippedFrames, int isFlushFrame, uint32_t *newBufferCapacity, void *custom);

/**
 * @brief Gets a free buffer pointer
 * @param[in] buffer the buffer to mark as free
 */
void ARSTREAMING_ReaderTb_SetBufferFree (uint8_t *buffer);

/**
 * @brief Gets a free buffer pointer
 * @param[out] retSize Pointer where to store the size of the next free buffer (or 0 if there is no free buffer)
 * @param[in] reallocToDouble Set to non-zero to realloc the buffer to the double of its previous size
 * @return Pointer to the next free buffer, or NULL if there is no free buffer
 */
uint8_t* ARSTREAMING_ReaderTb_GetNextFreeBuffer (uint32_t *retSize, int reallocToDouble);

/**
 * @brief Streaming entry point
 * @param manager An initialized network manager
 * @return "Main" return value
 */
int ARSTREAMING_ReaderTb_StartStreamingTest (ARNETWORK_Manager_t *manager, const char *outPath);

/*
 * Internal functions implementation
 */

void ARSTREAMING_ReaderTb_printUsage ()
{
    ARSAL_PRINT (ARSAL_PRINT_ERROR, __TAG__, "Usage : %s [ip] [outFile]", appName);
    ARSAL_PRINT (ARSAL_PRINT_ERROR, __TAG__, "        ip -> optionnal, ip of the stream sender");
    ARSAL_PRINT (ARSAL_PRINT_ERROR, __TAG__, "        outFile -> optionnal (ip must be provided), output file for received stream");
}

void ARSTREAMING_ReaderTb_initMultiBuffers (int initialSize)
{
    int buffIndex;
    for (buffIndex = 0; buffIndex < NB_BUFFERS; buffIndex++)
    {
        reallocBuffer (buffIndex, initialSize);
        multiBufferIsFree[buffIndex] = 1;
    }
}

void reallocBuffer (int id, int newSize)
{
    multiBuffer [id] = realloc (multiBuffer [id], newSize);
    multiBufferSize [id] = newSize;
}

uint8_t* ARSTREAMING_ReaderTb_FrameCompleteCallback (eARSTREAMING_READER_CAUSE cause, uint8_t *framePointer, uint32_t frameSize, int numberOfSkippedFrames, int isFlushFrame, uint32_t *newBufferCapacity, void *buffer)
{
    uint8_t *retVal = NULL;
    struct timeval now;
    int dt;
    buffer = buffer;
    switch (cause)
    {
    case ARSTREAMING_READER_CAUSE_FRAME_COMPLETE:
        ARSAL_PRINT (ARSAL_PRINT_WARNING, __TAG__, "Got a complete frame of size %d, at address %p (isFlush : %d)", frameSize, framePointer, isFlushFrame);
        if (isFlushFrame != 0)
        nbRead++;
        if (numberOfSkippedFrames != 0)
        {
            ARSAL_PRINT (ARSAL_PRINT_WARNING, __TAG__, "Skipped %d frames", numberOfSkippedFrames);
            if (numberOfSkippedFrames > 0)
            {
                nbSkipped += numberOfSkippedFrames;
                nbSkippedSinceLast += numberOfSkippedFrames;
            }
        }
        ARSTREAMING_Reader_PercentOk = (100.f * nbRead) / (1.f * (nbRead + nbSkipped));
        if (outFile != NULL)
        {
            fwrite (framePointer, 1, frameSize, outFile);
        }
        gettimeofday(&now, NULL);
        dt = ARSAL_Time_ComputeMsTimeDiff(&lastRecv, &now);
        lastDt [currentIndexInDt] = dt;
        currentIndexInDt ++;
        currentIndexInDt %= NB_FRAMES_FOR_AVERAGE;
        lastRecv.tv_sec = now.tv_sec;
        lastRecv.tv_usec = now.tv_usec;
        ARSTREAMING_ReaderTb_SetBufferFree (framePointer);
        retVal = ARSTREAMING_ReaderTb_GetNextFreeBuffer (newBufferCapacity, 0);
        break;

    case ARSTREAMING_READER_CAUSE_FRAME_TOO_SMALL:
        ARSAL_PRINT (ARSAL_PRINT_WARNING, __TAG__, "Current buffer is to small for frame !");
        retVal = ARSTREAMING_ReaderTb_GetNextFreeBuffer (newBufferCapacity, 1);
        break;

    case ARSTREAMING_READER_CAUSE_COPY_COMPLETE:
        ARSAL_PRINT (ARSAL_PRINT_WARNING, __TAG__, "Copy complete in new buffer, freeing this one");
        ARSTREAMING_ReaderTb_SetBufferFree (framePointer);
        break;

    case ARSTREAMING_READER_CAUSE_CANCEL:
        ARSAL_PRINT (ARSAL_PRINT_WARNING, __TAG__, "Reader is closing");
        ARSTREAMING_ReaderTb_SetBufferFree (framePointer);
        break;

    default:
        ARSAL_PRINT (ARSAL_PRINT_ERROR, __TAG__, "Unknown cause (probably a bug !)");
        break;
    }
    return retVal;
}

void ARSTREAMING_ReaderTb_SetBufferFree (uint8_t *buffer)
{
    int i;
    for (i = 0; i < NB_BUFFERS; i++)
    {
        if (multiBuffer[i] == buffer)
        {
            multiBufferIsFree[i] = 1;
        }
    }
}

uint8_t* ARSTREAMING_ReaderTb_GetNextFreeBuffer (uint32_t *retSize, int reallocToDouble)
{
    uint8_t *retBuffer = NULL;
    int nbtest = 0;
    if (retSize == NULL)
    {
        return NULL;
    }
    do
    {
        if (multiBufferIsFree[currentBufferIndex] == 1)
        {
            if (reallocToDouble != 0)
            {
                reallocBuffer (currentBufferIndex, 2* multiBufferSize [currentBufferIndex]);
            }
            retBuffer = multiBuffer[currentBufferIndex];
            *retSize = multiBufferSize[currentBufferIndex];
        }
        currentBufferIndex = (currentBufferIndex + 1) % NB_BUFFERS;
        nbtest++;
    } while (retBuffer == NULL && nbtest < NB_BUFFERS);
    return retBuffer;
}

int ARSTREAMING_ReaderTb_StartStreamingTest (ARNETWORK_Manager_t *manager, const char *outPath)
{
    int retVal = 0;

    uint8_t *firstFrame;
    uint32_t firstFrameSize;
    eARSTREAMING_ERROR err;
    if (NULL != outPath)
    {
        outFile = fopen (outPath, "wb");
    }
    else
    {
        outFile = NULL;
    }
    ARSTREAMING_ReaderTb_initMultiBuffers (FRAME_MAX_SIZE);
    ARSAL_Sem_Init (&closeSem, 0, 0);
    firstFrame = ARSTREAMING_ReaderTb_GetNextFreeBuffer (&firstFrameSize, 0);
    g_Reader = ARSTREAMING_Reader_New (manager, DATA_BUFFER_ID, ACK_BUFFER_ID, ARSTREAMING_ReaderTb_FrameCompleteCallback, firstFrame, firstFrameSize, NULL, &err);
    if (g_Reader == NULL)
    {
        ARSAL_PRINT (ARSAL_PRINT_ERROR, __TAG__, "Error during ARSTREAMING_Reader_New call : %s", ARSTREAMING_Error_ToString(err));
        return 1;
    }

    pthread_t streamingsend, streamingread;
    pthread_create (&streamingsend, NULL, ARSTREAMING_Reader_RunDataThread, g_Reader);
    pthread_create (&streamingread, NULL, ARSTREAMING_Reader_RunAckThread, g_Reader);

    /* USER CODE */

    running = 1;
    ARSAL_Sem_Wait (&closeSem);
    running = 0;

    /* END OF USER CODE */

    ARSTREAMING_Reader_StopReader (g_Reader);

    pthread_join (streamingread, NULL);
    pthread_join (streamingsend, NULL);

    ARSTREAMING_Reader_Delete (&g_Reader);

    ARSAL_Sem_Destroy (&closeSem);

    return retVal;
}

/*
 * Implementation
 */

int ARSTREAMING_Reader_TestBenchMain (int argc, char *argv[])
{
    int retVal = 0;
    appName = argv[0];
    if (argc > 3)
    {
        ARSTREAMING_ReaderTb_printUsage ();
        return 1;
    }

    char *ip = __IP;
    char *outPath = NULL;

    if (argc >= 2)
    {
        ip = argv[1];
    }
    if (argc >= 3)
    {
        outPath = argv[2];
    }

    ARSAL_PRINT (ARSAL_PRINT_WARNING, __TAG__, "IP = %s", ip);

    int nbInBuff = 1;
    ARNETWORK_IOBufferParam_t inParams;
    ARSTREAMING_Reader_InitStreamingAckBuffer (&inParams, ACK_BUFFER_ID);
    int nbOutBuff = 1;
    ARNETWORK_IOBufferParam_t outParams;
    ARSTREAMING_Reader_InitStreamingDataBuffer (&outParams, DATA_BUFFER_ID);

    eARNETWORK_ERROR error;
    eARNETWORKAL_ERROR specificError = ARNETWORKAL_OK;
    ARNETWORKAL_Manager_t *osspecificManagerPtr = ARNETWORKAL_Manager_New(&specificError);

    if(specificError == ARNETWORKAL_OK)
    {
        specificError = ARNETWORKAL_Manager_InitWifiNetwork(osspecificManagerPtr, ip, SENDING_PORT, READING_PORT, 1000);
    }
    else
    {
        ARSAL_PRINT (ARSAL_PRINT_ERROR, __TAG__, "Error during ARNETWORKAL_Manager_New call : %s", ARNETWORKAL_Error_ToString(specificError));
    }

    if(specificError == ARNETWORKAL_OK)
    {
        g_Manager = ARNETWORK_Manager_New(osspecificManagerPtr, nbInBuff, &inParams, nbOutBuff, &outParams, READER_PING_DELAY, &error);
    }
    else
    {
        ARSAL_PRINT (ARSAL_PRINT_ERROR, __TAG__, "Error during ARNETWORKAL_Manager_InitWifiNetwork call : %s", ARNETWORKAL_Error_ToString(specificError));
        error = ARNETWORK_ERROR;
    }

    if ((g_Manager == NULL) ||
        (error != ARNETWORK_OK))
    {
        ARSAL_PRINT (ARSAL_PRINT_ERROR, __TAG__, "Error during ARNETWORK_Manager_New call : %s", ARNETWORK_Error_ToString(error));
        return 1;
    }

    pthread_t netsend, netread;
    pthread_create (&netsend, NULL, ARNETWORK_Manager_SendingThreadRun, g_Manager);
    pthread_create (&netread, NULL, ARNETWORK_Manager_ReceivingThreadRun, g_Manager);

    retVal = ARSTREAMING_ReaderTb_StartStreamingTest (g_Manager, outPath);

    ARNETWORK_Manager_Stop (g_Manager);

    pthread_join (netread, NULL);
    pthread_join (netsend, NULL);

    ARNETWORK_Manager_Delete (&g_Manager);
    ARNETWORKAL_Manager_CloseWifiNetwork(osspecificManagerPtr);
    ARNETWORKAL_Manager_Delete(&osspecificManagerPtr);

    return retVal;
}

void ARSTREAMING_Reader_TestBenchStop ()
{
    if (running)
    {
        ARSAL_Sem_Post (&closeSem);
    }
}

int ARSTREAMING_ReaderTb_GetMeanTimeBetweenFrames ()
{
    int retVal = 0;
    int i;
    for (i = 0; i < NB_FRAMES_FOR_AVERAGE; i++)
    {
        retVal += lastDt [i];
    }
    return retVal / NB_FRAMES_FOR_AVERAGE;
}


int ARSTREAMING_ReaderTb_GetLatency ()
{
    if (g_Manager == NULL)
    {
        return -1;
    }
    else
    {
        return ARNETWORK_Manager_GetEstimatedLatency(g_Manager);
    }
}

int ARSTREAMING_ReaderTb_GetMissedFrames ()
{
    int retval = nbSkippedSinceLast;
    nbSkippedSinceLast = 0;
    return retval;
}

float ARSTREAMING_ReaderTb_GetEfficiency ()
{
    if (g_Reader == NULL)
    {
        return 0.f;
    }
    return ARSTREAMING_Reader_GetEstimatedEfficiency(g_Reader);
}

int ARSTREAMING_ReaderTb_GetEstimatedLoss ()
{
    if (g_Manager == NULL)
    {
        return 100;
    }
    return ARNETWORK_Manager_GetEstimatedMissPercentage(g_Manager, DATA_BUFFER_ID);
}