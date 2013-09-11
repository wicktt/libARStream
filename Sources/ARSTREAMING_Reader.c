/**
 * @file ARSTREAMING_Reader.c
 * @brief Streaming reader on network
 * @date 03/22/2013
 * @author nicolas.brulez@parrot.com
 */

#include <config.h>

/*
 * System Headers
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Private Headers
 */

#include "ARSTREAMING_Buffers.h"
#include "ARSTREAMING_NetworkHeaders.h"

/*
 * ARSDK Headers
 */

#include <libARStreaming/ARSTREAMING_Reader.h>
#include <libARSAL/ARSAL_Print.h>
#include <libARSAL/ARSAL_Mutex.h>
#include <libARSAL/ARSAL_Endianness.h>

/*
 * Macros
 */

#define ARSTREAMING_READER_TAG "ARSTREAMING_Reader"
#define ARSTREAMING_READER_DATAREAD_TIMEOUT_MS (500)
#define ARSTREAMING_READER_MAX_TIME_BETWEEN_ACK_MS (5)

#define ARSTREAMING_READER_EFFICIENCY_AVERAGE_NB_FRAMES (15)

/**
 * Sets *PTR to VAL if PTR is not null
 */
#define SET_WITH_CHECK(PTR,VAL)                 \
    do                                          \
    {                                           \
        if (PTR != NULL)                        \
        {                                       \
            *PTR = VAL;                         \
        }                                       \
    } while (0)

/*
 * Types
 */

struct ARSTREAMING_Reader_t {
    /* Configuration on New */
    ARNETWORK_Manager_t *manager;
    int dataBufferID;
    int ackBufferID;
    ARSTREAMING_Reader_FrameCompleteCallback_t callback;
    void *custom;

    /* Current frame storage */
    uint32_t currentFrameBufferSize; // Usable length of the buffer
    uint32_t currentFrameSize;       // Actual data length
    uint8_t *currentFrameBuffer;

    /* Acknowledge storage */
    ARSAL_Mutex_t ackPacketMutex;
    ARSTREAMING_NetworkHeaders_AckPacket_t ackPacket;
    ARSAL_Mutex_t ackSendMutex;
    ARSAL_Cond_t ackSendCond;

    /* Thread status */
    int threadsShouldStop;
    int dataThreadStarted;
    int ackThreadStarted;

    /* Efficiency calculations */
    int efficiency_nbUseful [ARSTREAMING_READER_EFFICIENCY_AVERAGE_NB_FRAMES];
    int efficiency_nbTotal  [ARSTREAMING_READER_EFFICIENCY_AVERAGE_NB_FRAMES];
    int efficiency_index;
};

/*
 * Internal functions declarations
 */

/**
 * @brief ARNETWORK_Manager_Callback_t for ARNETWORK_... calls
 * @param IoBufferId Unused as we always send on one unique buffer
 * @param dataPtr Unused as we don't need to free the memory
 * @param customData Unused
 * @param status Unused
 * @return ARNETWORK_MANAGER_CALLBACK_RETURN_DEFAULT
 */
eARNETWORK_MANAGER_CALLBACK_RETURN ARSTREAMING_Reader_NetworkCallback (int IoBufferId, uint8_t *dataPtr, void *customData, eARNETWORK_MANAGER_CALLBACK_STATUS status);

/*
 * Internal functions implementation
 */

//TODO: Network, NULL callback should be ok ?
eARNETWORK_MANAGER_CALLBACK_RETURN ARSTREAMING_Reader_NetworkCallback (int IoBufferId, uint8_t *dataPtr, void *customData, eARNETWORK_MANAGER_CALLBACK_STATUS status)
{
    /* Avoid "unused parameters" warnings */
    IoBufferId = IoBufferId;
    dataPtr = dataPtr;
    customData = customData;
    status = status;

    /* Dummy return value */
    return ARNETWORK_MANAGER_CALLBACK_RETURN_DEFAULT;
}

/*
 * Implementation
 */

void ARSTREAMING_Reader_InitStreamingDataBuffer (ARNETWORK_IOBufferParam_t *bufferParams, int bufferID)
{
    ARSTREAMING_Buffers_InitStreamingDataBuffer (bufferParams, bufferID);
}

void ARSTREAMING_Reader_InitStreamingAckBuffer (ARNETWORK_IOBufferParam_t *bufferParams, int bufferID)
{
    ARSTREAMING_Buffers_InitStreamingAckBuffer (bufferParams, bufferID);
}

ARSTREAMING_Reader_t* ARSTREAMING_Reader_New (ARNETWORK_Manager_t *manager, int dataBufferID, int ackBufferID, ARSTREAMING_Reader_FrameCompleteCallback_t callback, uint8_t *frameBuffer, uint32_t frameBufferSize, void *custom, eARSTREAMING_ERROR *error)
{
    ARSTREAMING_Reader_t *retReader = NULL;
    int ackPacketMutexWasInit = 0;
    int ackSendMutexWasInit = 0;
    int ackSendCondWasInit = 0;
    eARSTREAMING_ERROR internalError = ARSTREAMING_OK;
    /* ARGS Check */
    if ((manager == NULL) ||
        (callback == NULL) ||
        (frameBuffer == NULL) ||
        (frameBufferSize == 0))
    {
        SET_WITH_CHECK (error, ARSTREAMING_ERROR_BAD_PARAMETERS);
        return retReader;
    }

    /* Alloc new reader */
    retReader = malloc (sizeof (ARSTREAMING_Reader_t));
    if (retReader == NULL)
    {
        internalError = ARSTREAMING_ERROR_ALLOC;
    }

    /* Copy parameters */
    if (internalError == ARSTREAMING_OK)
    {
        retReader->manager = manager;
        retReader->dataBufferID = dataBufferID;
        retReader->ackBufferID = ackBufferID;
        retReader->callback = callback;
        retReader->custom = custom;
        retReader->currentFrameBufferSize = frameBufferSize;
        retReader->currentFrameBuffer = frameBuffer;
    }

    /* Setup internal mutexes/conditions */
    if (internalError == ARSTREAMING_OK)
    {
        int mutexInitRet = ARSAL_Mutex_Init (&(retReader->ackPacketMutex));
        if (mutexInitRet != 0)
        {
            internalError = ARSTREAMING_ERROR_ALLOC;
        }
        else
        {
            ackPacketMutexWasInit = 1;
        }
    }
    if (internalError == ARSTREAMING_OK)
    {
        int mutexInitRet = ARSAL_Mutex_Init (&(retReader->ackSendMutex));
        if (mutexInitRet != 0)
        {
            internalError = ARSTREAMING_ERROR_ALLOC;
        }
        else
        {
            ackSendMutexWasInit = 1;
        }
    }
    if (internalError == ARSTREAMING_OK)
    {
        int condInitRet = ARSAL_Cond_Init (&(retReader->ackSendCond));
        if (condInitRet != 0)
        {
            internalError = ARSTREAMING_ERROR_ALLOC;
        }
        else
        {
            ackSendCondWasInit = 1;
        }
    }

    /* Setup internal variables */
    if (internalError == ARSTREAMING_OK)
    {
        int i;
        retReader->currentFrameSize = 0;
        retReader->threadsShouldStop = 0;
        retReader->dataThreadStarted = 0;
        retReader->ackThreadStarted = 0;
        retReader->efficiency_index = 0;
        for (i = 0; i < ARSTREAMING_READER_EFFICIENCY_AVERAGE_NB_FRAMES; i++)
        {
            retReader->efficiency_nbTotal [i] = 0;
            retReader->efficiency_nbUseful [i] = 0;
        }
    }

    if ((internalError != ARSTREAMING_OK) &&
        (retReader != NULL))
    {
        if (ackPacketMutexWasInit == 1)
        {
            ARSAL_Mutex_Destroy (&(retReader->ackPacketMutex));
        }
        if (ackSendMutexWasInit == 1)
        {
            ARSAL_Mutex_Destroy (&(retReader->ackSendMutex));
        }
        if (ackSendCondWasInit == 1)
        {
            ARSAL_Cond_Destroy (&(retReader->ackSendCond));
        }
        free (retReader);
        retReader = NULL;
    }

    SET_WITH_CHECK (error, internalError);
    return retReader;
}

void ARSTREAMING_Reader_StopReader (ARSTREAMING_Reader_t *reader)
{
    if (reader != NULL)
    {
        reader->threadsShouldStop = 1;
    }
}

eARSTREAMING_ERROR ARSTREAMING_Reader_Delete (ARSTREAMING_Reader_t **reader)
{
    eARSTREAMING_ERROR retVal = ARSTREAMING_ERROR_BAD_PARAMETERS;
    if ((reader != NULL) &&
        (*reader != NULL))
    {
        int canDelete = 0;
        if (((*reader)->dataThreadStarted == 0) &&
            ((*reader)->ackThreadStarted == 0))
        {
            canDelete = 1;
        }

        if (canDelete == 1)
        {
            ARSAL_Mutex_Destroy (&((*reader)->ackPacketMutex));
            ARSAL_Mutex_Destroy (&((*reader)->ackSendMutex));
            ARSAL_Cond_Destroy (&((*reader)->ackSendCond));
            free (*reader);
            *reader = NULL;
            retVal = ARSTREAMING_OK;
        }
        else
        {
            ARSAL_PRINT (ARSAL_PRINT_ERROR, ARSTREAMING_READER_TAG, "Call ARSTREAMING_Reader_StopReader before calling this function");
            retVal = ARSTREAMING_ERROR_BUSY;
        }
    }
    return retVal;
}

void* ARSTREAMING_Reader_RunDataThread (void *ARSTREAMING_Reader_t_Param)
{
    int recvDataLen = ARSTREAMING_NETWORK_HEADERS_FRAGMENT_SIZE + sizeof (ARSTREAMING_NetworkHeaders_DataHeader_t);
    uint8_t *recvData = NULL;
    int recvSize;
    uint16_t previousFNum = UINT16_MAX;
    int skipCurrentFrame = 0;
    int packetWasAlreadyAck = 0;
    ARSTREAMING_Reader_t *reader = (ARSTREAMING_Reader_t *)ARSTREAMING_Reader_t_Param;
    ARSTREAMING_NetworkHeaders_DataHeader_t *header = NULL;

    /* Parameters check */
    if (reader == NULL)
    {
        ARSAL_PRINT (ARSAL_PRINT_ERROR, ARSTREAMING_READER_TAG, "Error while starting %s, bad parameters", __FUNCTION__);
        return (void *)0;
    }

    /* Alloc and check */
    recvData = malloc (recvDataLen);
    if (recvData == NULL)
    {
        ARSAL_PRINT (ARSAL_PRINT_ERROR, ARSTREAMING_READER_TAG, "Error while starting %s, can not alloc memory", __FUNCTION__);
        return (void *)0;
    }
    header = (ARSTREAMING_NetworkHeaders_DataHeader_t *)recvData;

    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARSTREAMING_READER_TAG, "Streaming reader thread running");
    reader->dataThreadStarted = 1;

    while (reader->threadsShouldStop == 0)
    {
        eARNETWORK_ERROR err = ARNETWORK_Manager_ReadDataWithTimeout (reader->manager, reader->dataBufferID, recvData, recvDataLen, &recvSize, ARSTREAMING_READER_DATAREAD_TIMEOUT_MS);
        if (ARNETWORK_OK != err)
        {
            if (ARNETWORK_ERROR_BUFFER_EMPTY != err)
            {
                ARSAL_PRINT (ARSAL_PRINT_ERROR, ARSTREAMING_READER_TAG, "Error while reading stream data: %s", ARNETWORK_Error_ToString (err));
            }
        }
        else
        {
            int cpIndex, cpSize, endIndex;
            ARSAL_Mutex_Lock (&(reader->ackPacketMutex));
            if (header->frameNumber != reader->ackPacket.frameNumber)
            {
                reader->efficiency_index ++;
                reader->efficiency_index %= ARSTREAMING_READER_EFFICIENCY_AVERAGE_NB_FRAMES;
                reader->efficiency_nbTotal [reader->efficiency_index] = 0;
                reader->efficiency_nbUseful [reader->efficiency_index] = 0;
                skipCurrentFrame = 0;
                reader->currentFrameSize = 0;
                reader->ackPacket.frameNumber = header->frameNumber;
#ifdef DEBUG
                uint32_t nackPackets = ARSTREAMING_NetworkHeaders_AckPacketCountNotSet (&(reader->ackPacket), header->fragmentsPerFrame);
                if (nackPackets != 0)
                {
                    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARSTREAMING_READER_TAG, "Dropping a frame (missing %d fragments)", nackPackets);
                }
#endif
                ARSTREAMING_NetworkHeaders_AckPacketReset (&(reader->ackPacket));
            }
            packetWasAlreadyAck = ARSTREAMING_NetworkHeaders_AckPacketFlagIsSet (&(reader->ackPacket), header->fragmentNumber);
            ARSTREAMING_NetworkHeaders_AckPacketSetFlag (&(reader->ackPacket), header->fragmentNumber);

            reader->efficiency_nbTotal [reader->efficiency_index] ++;
            if (packetWasAlreadyAck == 0)
            {
                reader->efficiency_nbUseful [reader->efficiency_index] ++;
            }

            ARSAL_Mutex_Unlock (&(reader->ackPacketMutex));

            ARSAL_Mutex_Lock (&(reader->ackSendMutex));
            ARSAL_Cond_Signal (&(reader->ackSendCond));
            ARSAL_Mutex_Unlock (&(reader->ackSendMutex));


            cpIndex = ARSTREAMING_NETWORK_HEADERS_FRAGMENT_SIZE*header->fragmentNumber;
            cpSize = recvSize - sizeof (ARSTREAMING_NetworkHeaders_DataHeader_t);
            endIndex = cpIndex + cpSize;
            while ((endIndex > reader->currentFrameBufferSize) &&
                   (skipCurrentFrame == 0))
            {
                uint32_t nextFrameBufferSize = 0;
                uint32_t dummy;
                uint8_t *nextFrameBuffer = reader->callback (ARSTREAMING_READER_CAUSE_FRAME_TOO_SMALL, reader->currentFrameBuffer, reader->currentFrameSize, 0, 0, &nextFrameBufferSize, reader->custom);
                if (nextFrameBufferSize >= reader->currentFrameSize && nextFrameBufferSize > 0)
                {
                    memcpy (nextFrameBuffer, reader->currentFrameBuffer, reader->currentFrameSize);
                }
                else
                {
                    skipCurrentFrame = 1;
                }
                //TODO: Add "SKIP_FRAME"
                reader->callback (ARSTREAMING_READER_CAUSE_COPY_COMPLETE, reader->currentFrameBuffer, reader->currentFrameSize, 0, 0, &dummy, reader->custom);
                reader->currentFrameBuffer = nextFrameBuffer;
                reader->currentFrameBufferSize = nextFrameBufferSize;
            }

            if (skipCurrentFrame == 0)
            {
                if (packetWasAlreadyAck == 0)
                {
                    memcpy (&(reader->currentFrameBuffer)[cpIndex], &recvData[sizeof (ARSTREAMING_NetworkHeaders_DataHeader_t)], recvSize - sizeof (ARSTREAMING_NetworkHeaders_DataHeader_t));
                }

                if (endIndex > reader->currentFrameSize)
                {
                    reader->currentFrameSize = endIndex;
                }

                ARSAL_Mutex_Lock (&(reader->ackPacketMutex));
                if (ARSTREAMING_NetworkHeaders_AckPacketAllFlagsSet (&(reader->ackPacket), header->fragmentsPerFrame))
                {
                    if (header->frameNumber != previousFNum)
                    {
                        int nbMissedFrame = 0;
                        int isFlushFrame = ((header->frameFlags & ARSTREAMING_NETWORK_HEADERS_FLAG_FLUSH_FRAME) != 0) ? 1 : 0;
                        ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARSTREAMING_READER_TAG, "Ack all in frame %d", header->frameNumber);
                        if (header->frameNumber != previousFNum + 1)
                        {
                            nbMissedFrame = header->frameNumber - previousFNum - 1;
                            ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARSTREAMING_READER_TAG, "Missed %d frames !", nbMissedFrame);
                        }
                        previousFNum = header->frameNumber;
                        reader->currentFrameBuffer = reader->callback (ARSTREAMING_READER_CAUSE_FRAME_COMPLETE, reader->currentFrameBuffer, reader->currentFrameSize, nbMissedFrame, isFlushFrame, &(reader->currentFrameBufferSize), reader->custom);
                    }
                }
                ARSAL_Mutex_Unlock (&(reader->ackPacketMutex));
            }
        }
    }

    free (recvData);

    reader->callback (ARSTREAMING_READER_CAUSE_CANCEL, reader->currentFrameBuffer, reader->currentFrameSize, 0, 0, &(reader->currentFrameBufferSize), reader->custom);

    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARSTREAMING_READER_TAG, "Streaming reader thread ended");
    reader->dataThreadStarted = 0;
    return (void *)0;
}

void* ARSTREAMING_Reader_RunAckThread (void *ARSTREAMING_Reader_t_Param)
{
    ARSTREAMING_NetworkHeaders_AckPacket_t sendPacket = {0};
    ARSTREAMING_Reader_t *reader = (ARSTREAMING_Reader_t *)ARSTREAMING_Reader_t_Param;

    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARSTREAMING_READER_TAG, "Ack sender thread running");
    reader->ackThreadStarted = 1;

    while (reader->threadsShouldStop == 0)
    {
        ARSAL_Mutex_Lock (&(reader->ackSendMutex));
        ARSAL_Cond_Timedwait (&(reader->ackSendCond), &(reader->ackSendMutex), ARSTREAMING_READER_MAX_TIME_BETWEEN_ACK_MS);
        ARSAL_Mutex_Unlock (&(reader->ackSendMutex));
        ARSAL_Mutex_Lock (&(reader->ackPacketMutex));
        sendPacket.frameNumber = htods  (reader->ackPacket.frameNumber);
        sendPacket.highPacketsAck = htodll (reader->ackPacket.highPacketsAck);
        sendPacket.lowPacketsAck  = htodll (reader->ackPacket.lowPacketsAck);
        ARSAL_Mutex_Unlock (&(reader->ackPacketMutex));

        ARNETWORK_Manager_SendData (reader->manager, reader->ackBufferID, (uint8_t *)&sendPacket, sizeof (sendPacket), NULL, ARSTREAMING_Reader_NetworkCallback, 1);
    }

    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARSTREAMING_READER_TAG, "Ack sender thread ended");
    reader->ackThreadStarted = 0;
    return (void *)0;
}

float ARSTREAMING_Reader_GetEstimatedEfficiency (ARSTREAMING_Reader_t *reader)
{
    float retVal = 1.0f;
    uint32_t totalPackets = 0;
    uint32_t usefulPackets = 0;
    int i;
    ARSAL_Mutex_Lock (&(reader->ackPacketMutex));
    for (i = 0; i < ARSTREAMING_READER_EFFICIENCY_AVERAGE_NB_FRAMES; i++)
    {
        totalPackets += reader->efficiency_nbTotal [i];
        usefulPackets += reader->efficiency_nbUseful [i];
    }
    ARSAL_Mutex_Unlock (&(reader->ackPacketMutex));
    if (totalPackets == 0)
    {
        retVal = 0.0f; // We didn't receive anything yet ... not really efficient
    }
    else if (usefulPackets > totalPackets)
    {
        retVal = 1.0f; // If this happens, it means that we have a big problem
        ARSAL_PRINT (ARSAL_PRINT_ERROR, ARSTREAMING_READER_TAG, "Computed efficiency is greater that 1.0 ...");
    }
    else
    {
        retVal = (1.f * usefulPackets) / (1.f * totalPackets);
    }
    return retVal;
}