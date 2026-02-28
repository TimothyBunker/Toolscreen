#include "practice_world_launch.h"

#include "utils.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#if defined(TOOLSCREEN_HAS_JNI) && TOOLSCREEN_HAS_JNI
#include <jni.h>
#endif

namespace {

constexpr UINT kPracticeWorldLaunchMessage = WM_APP + 0x5A1;

std::mutex g_pendingPracticeLaunchMutex;
bool g_hasPendingPracticeLaunch = false;
std::string g_pendingPracticeWorldName;

#if defined(TOOLSCREEN_HAS_JNI) && TOOLSCREEN_HAS_JNI
using JniGetCreatedJavaVMsFn = jint(JNICALL*)(JavaVM**, jsize, jsize*);

bool TryLaunchWorldViaJvmDirect(const std::string& worldName, std::string& outError) {
    outError.clear();

    HMODULE jvmModule = GetModuleHandleW(L"jvm.dll");
    if (!jvmModule) {
        outError = "jvm.dll is not loaded in process.";
        return false;
    }

    auto* getCreatedVmFn = reinterpret_cast<JniGetCreatedJavaVMsFn>(GetProcAddress(jvmModule, "JNI_GetCreatedJavaVMs"));
    if (!getCreatedVmFn) {
        outError = "JNI_GetCreatedJavaVMs not found.";
        return false;
    }

    JavaVM* vm = nullptr;
    jsize vmCount = 0;
    if (getCreatedVmFn(&vm, 1, &vmCount) != JNI_OK || vm == nullptr || vmCount < 1) {
        outError = "No active Java VM found.";
        return false;
    }

    JNIEnv* env = nullptr;
    bool attachedHere = false;
    jint getEnvResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
    if (getEnvResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK || env == nullptr) {
            outError = "AttachCurrentThread failed.";
            return false;
        }
        attachedHere = true;
    } else if (getEnvResult != JNI_OK || env == nullptr) {
        outError = "GetEnv failed.";
        return false;
    }

    auto clearException = [&](const char* stage) -> bool {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        outError = std::string("Java exception at stage: ") + stage;
        return true;
    };
    auto clearExceptionSoft = [&]() -> bool {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    };
    auto toBinaryClassName = [](std::string internalName) {
        std::replace(internalName.begin(), internalName.end(), '/', '.');
        return internalName;
    };
    auto tryResolveClass = [&](const std::vector<std::string>& internalCandidates, std::string& outSelectedInternal) -> jclass {
        outSelectedInternal.clear();

        for (const auto& internalName : internalCandidates) {
            jclass cls = env->FindClass(internalName.c_str());
            if (clearExceptionSoft()) cls = nullptr;
            if (cls) {
                outSelectedInternal = internalName;
                return cls;
            }
        }

        jclass threadClass = env->FindClass("java/lang/Thread");
        if (!threadClass || clearException("FindClass(java/lang/Thread)")) return nullptr;
        jmethodID currentThreadMid = env->GetStaticMethodID(threadClass, "currentThread", "()Ljava/lang/Thread;");
        if (!currentThreadMid || clearException("GetStaticMethodID(Thread.currentThread)")) {
            env->DeleteLocalRef(threadClass);
            return nullptr;
        }
        jobject threadObj = env->CallStaticObjectMethod(threadClass, currentThreadMid);
        if (clearException("CallStaticObjectMethod(Thread.currentThread)")) {
            env->DeleteLocalRef(threadClass);
            return nullptr;
        }
        jmethodID getContextLoaderMid = env->GetMethodID(threadClass, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
        if (!getContextLoaderMid || clearException("GetMethodID(Thread.getContextClassLoader)")) {
            if (threadObj) env->DeleteLocalRef(threadObj);
            env->DeleteLocalRef(threadClass);
            return nullptr;
        }
        jobject classLoaderObj = nullptr;
        if (threadObj) {
            classLoaderObj = env->CallObjectMethod(threadObj, getContextLoaderMid);
            if (clearException("CallObjectMethod(Thread.getContextClassLoader)")) classLoaderObj = nullptr;
        }

        jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
        if (!classLoaderClass || clearException("FindClass(java/lang/ClassLoader)")) {
            if (classLoaderObj) env->DeleteLocalRef(classLoaderObj);
            if (threadObj) env->DeleteLocalRef(threadObj);
            env->DeleteLocalRef(threadClass);
            return nullptr;
        }

        if (!classLoaderObj) {
            jmethodID getSystemLoaderMid =
                env->GetStaticMethodID(classLoaderClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
            if (!getSystemLoaderMid || clearException("GetStaticMethodID(ClassLoader.getSystemClassLoader)")) {
                env->DeleteLocalRef(classLoaderClass);
                if (threadObj) env->DeleteLocalRef(threadObj);
                env->DeleteLocalRef(threadClass);
                return nullptr;
            }
            classLoaderObj = env->CallStaticObjectMethod(classLoaderClass, getSystemLoaderMid);
            if (clearException("CallStaticObjectMethod(ClassLoader.getSystemClassLoader)")) classLoaderObj = nullptr;
        }

        if (!classLoaderObj) {
            env->DeleteLocalRef(classLoaderClass);
            if (threadObj) env->DeleteLocalRef(threadObj);
            env->DeleteLocalRef(threadClass);
            outError = "ClassLoader was null.";
            return nullptr;
        }

        jmethodID loadClassMid = env->GetMethodID(classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (!loadClassMid || clearException("GetMethodID(ClassLoader.loadClass)")) {
            env->DeleteLocalRef(classLoaderObj);
            env->DeleteLocalRef(classLoaderClass);
            if (threadObj) env->DeleteLocalRef(threadObj);
            env->DeleteLocalRef(threadClass);
            return nullptr;
        }

        jclass resolved = nullptr;
        for (const auto& internalName : internalCandidates) {
            const std::string binaryName = toBinaryClassName(internalName);
            jstring classNameJ = env->NewStringUTF(binaryName.c_str());
            if (!classNameJ || clearExceptionSoft()) continue;
            jobject clsObj = env->CallObjectMethod(classLoaderObj, loadClassMid, classNameJ);
            const bool hadEx = clearExceptionSoft();
            env->DeleteLocalRef(classNameJ);
            if (hadEx || !clsObj) continue;
            resolved = reinterpret_cast<jclass>(clsObj);
            outSelectedInternal = internalName;
            break;
        }

        env->DeleteLocalRef(classLoaderObj);
        env->DeleteLocalRef(classLoaderClass);
        if (threadObj) env->DeleteLocalRef(threadObj);
        env->DeleteLocalRef(threadClass);
        return resolved;
    };

    bool success = false;
    do {
        const std::vector<std::string> classCandidates = { "net/minecraft/class_310", "dlx", "net/minecraft/client/MinecraftClient" };
        const std::vector<std::string> getInstanceNameCandidates = { "B", "method_1551", "getInstance" };
        const std::vector<std::string> launchNameCandidates = { "a", "method_29606", "startIntegratedServer" };

        std::string selectedClassName;
        jclass minecraftClass = tryResolveClass(classCandidates, selectedClassName);
        if (!minecraftClass) {
            if (outError.empty()) outError = "Could not resolve MinecraftClient class via FindClass or ClassLoader.";
            break;
        }

        const std::string selfDesc = "()L" + selectedClassName + ";";
        jmethodID getInstanceMethod = nullptr;
        for (const auto& name : getInstanceNameCandidates) {
            getInstanceMethod = env->GetStaticMethodID(minecraftClass, name.c_str(), selfDesc.c_str());
            if (clearExceptionSoft()) {
                getInstanceMethod = nullptr;
                continue;
            }
            if (getInstanceMethod) break;
        }
        if (!getInstanceMethod) {
            env->DeleteLocalRef(minecraftClass);
            if (outError.empty()) outError = "Could not resolve MinecraftClient#getInstance.";
            break;
        }

        jobject minecraftClient = env->CallStaticObjectMethod(minecraftClass, getInstanceMethod);
        if (clearException("CallStaticObjectMethod(getInstance)")) {
            env->DeleteLocalRef(minecraftClass);
            break;
        }
        if (!minecraftClient) {
            env->DeleteLocalRef(minecraftClass);
            outError = "MinecraftClient#getInstance returned null.";
            break;
        }

        jmethodID launchMethod = nullptr;
        for (const auto& name : launchNameCandidates) {
            launchMethod = env->GetMethodID(minecraftClass, name.c_str(), "(Ljava/lang/String;)V");
            if (clearExceptionSoft()) {
                launchMethod = nullptr;
                continue;
            }
            if (launchMethod) break;
        }
        if (!launchMethod) {
            env->DeleteLocalRef(minecraftClient);
            env->DeleteLocalRef(minecraftClass);
            if (outError.empty()) outError = "Could not resolve MinecraftClient world-launch method.";
            break;
        }

        jstring worldNameJ = env->NewStringUTF(worldName.c_str());
        if (clearException("NewStringUTF")) {
            env->DeleteLocalRef(minecraftClient);
            env->DeleteLocalRef(minecraftClass);
            break;
        }
        if (!worldNameJ) {
            env->DeleteLocalRef(minecraftClient);
            env->DeleteLocalRef(minecraftClass);
            outError = "Failed to create Java world-name string.";
            break;
        }

        env->CallVoidMethod(minecraftClient, launchMethod, worldNameJ);
        if (clearException("CallVoidMethod(startIntegratedServer)")) {
            env->DeleteLocalRef(worldNameJ);
            env->DeleteLocalRef(minecraftClient);
            env->DeleteLocalRef(minecraftClass);
            break;
        }

        env->DeleteLocalRef(worldNameJ);
        env->DeleteLocalRef(minecraftClient);
        env->DeleteLocalRef(minecraftClass);
        success = true;
    } while (false);

    if (attachedHere) vm->DetachCurrentThread();
    return success;
}
#else
bool TryLaunchWorldViaJvmDirect(const std::string& worldName, std::string& outError) {
    (void)worldName;
    outError = "JNI support was not compiled into this build.";
    return false;
}
#endif

} // namespace

UINT GetPracticeWorldLaunchMessageId() { return kPracticeWorldLaunchMessage; }

bool QueuePracticeWorldLaunchRequest(const std::string& worldName, std::string* outError) {
    if (worldName.empty()) {
        if (outError) *outError = "World name is empty.";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_pendingPracticeLaunchMutex);
    g_pendingPracticeWorldName = worldName;
    g_hasPendingPracticeLaunch = true;
    if (outError) outError->clear();
    return true;
}

bool TryHandlePracticeWorldLaunchWindowMessage(HWND hWnd, UINT uMsg, LRESULT& outResult) {
    (void)hWnd;
    if (uMsg != kPracticeWorldLaunchMessage) return false;

    std::string worldName;
    {
        std::lock_guard<std::mutex> lock(g_pendingPracticeLaunchMutex);
        if (!g_hasPendingPracticeLaunch || g_pendingPracticeWorldName.empty()) {
            outResult = 0;
            return true;
        }
        worldName = g_pendingPracticeWorldName;
        g_pendingPracticeWorldName.clear();
        g_hasPendingPracticeLaunch = false;
    }

    std::string error;
    const bool launched = TryLaunchWorldViaJvmDirect(worldName, error);
    if (launched) {
        Log("[Practice] Direct world launch requested for '" + worldName + "'.");
        outResult = 1;
        return true;
    }

    Log("[Practice] Direct world launch failed for '" + worldName + "': " + error);
    outResult = 0;
    return true;
}
