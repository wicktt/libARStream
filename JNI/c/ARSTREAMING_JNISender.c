#include <jni.h>
#include <libARStreaming/ARSTREAMING_Sender.h>
#include <libARSAL/ARSAL_Print.h>

#define JNI_SENDER_TAG "ARSTREAMING_JNISender"

static jmethodID g_cbWrapper_id = 0;
static JavaVM *g_vm = NULL;