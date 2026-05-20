// Copyright Odyssey Interactive. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "BlueprintBridgeInternal.h"
#include "HAL/Runnable.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/RunnableThread.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintBridge, Log, All);

static FString JsonToString(const TSharedRef<FJsonObject>& JsonObject)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(JsonObject, Writer);
	return Output;
}

namespace BlueprintBridge
{
#if PLATFORM_WINDOWS
class FNamedPipeServer final : public FRunnable
{
public:
	FNamedPipeServer()
		: PipePath(GetPipeNamePath())
	{
	}

	~FNamedPipeServer() override
	{
		Stop();
	}

	bool Start()
	{
		Thread = FRunnableThread::Create(this, TEXT("BlueprintBridgeNamedPipe"));
		return Thread != nullptr;
	}

	void Stop() override
	{
		bStopRequested = true;

		HANDLE WakeHandle = CreateFileW(*PipePath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (WakeHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(WakeHandle);
		}

		if (PipeHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(PipeHandle);
			PipeHandle = INVALID_HANDLE_VALUE;
		}

		if (Thread)
		{
			Thread->WaitForCompletion();
			delete Thread;
			Thread = nullptr;
		}
	}

	uint32 Run() override
	{
		while (!bStopRequested)
		{
			PipeHandle = CreateNamedPipeW(
				*PipePath,
				PIPE_ACCESS_DUPLEX,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
				1,
				1024 * 1024,
				1024 * 1024,
				0,
				nullptr);

			if (PipeHandle == INVALID_HANDLE_VALUE)
			{
				UE_LOG(LogBlueprintBridge, Error, TEXT("CreateNamedPipe failed: %lu"), GetLastError());
				FPlatformProcess::Sleep(1.0f);
				continue;
			}

			const bool bConnected = ConnectNamedPipe(PipeHandle, nullptr) ? true : GetLastError() == ERROR_PIPE_CONNECTED;
			if (bConnected && !bStopRequested)
			{
				HandleClient(PipeHandle);
			}

			FlushFileBuffers(PipeHandle);
			DisconnectNamedPipe(PipeHandle);
			CloseHandle(PipeHandle);
			PipeHandle = INVALID_HANDLE_VALUE;
		}

		return 0;
	}

private:
	static bool ReadExact(HANDLE Handle, void* Buffer, const uint32 BytesToRead)
	{
		uint8* Cursor = static_cast<uint8*>(Buffer);
		uint32 TotalRead = 0;
		while (TotalRead < BytesToRead)
		{
			DWORD BytesRead = 0;
			if (!ReadFile(Handle, Cursor + TotalRead, BytesToRead - TotalRead, &BytesRead, nullptr) || BytesRead == 0)
			{
				return false;
			}
			TotalRead += BytesRead;
		}
		return true;
	}

	static bool WriteExact(HANDLE Handle, const void* Buffer, const uint32 BytesToWrite)
	{
		const uint8* Cursor = static_cast<const uint8*>(Buffer);
		uint32 TotalWritten = 0;
		while (TotalWritten < BytesToWrite)
		{
			DWORD BytesWritten = 0;
			if (!WriteFile(Handle, Cursor + TotalWritten, BytesToWrite - TotalWritten, &BytesWritten, nullptr) || BytesWritten == 0)
			{
				return false;
			}
			TotalWritten += BytesWritten;
		}
		return true;
	}

	static void HandleClient(HANDLE ClientHandle)
	{
		while (true)
		{
			uint32 PayloadSize = 0;
			if (!ReadExact(ClientHandle, &PayloadSize, sizeof(PayloadSize)) || PayloadSize == 0 || PayloadSize > 16 * 1024 * 1024)
			{
				return;
			}

			TArray<uint8> Payload;
			Payload.SetNumUninitialized(PayloadSize + 1);
			if (!ReadExact(ClientHandle, Payload.GetData(), PayloadSize))
			{
				return;
			}
			Payload[PayloadSize] = 0;

			const FString RequestText = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Payload.GetData())));
			const FString ResponseText = JsonToString(ExecuteRequest(RequestText));
			FTCHARToUTF8 ResponseUtf8(*ResponseText);
			const uint32 ResponseSize = ResponseUtf8.Length();
			if (!WriteExact(ClientHandle, &ResponseSize, sizeof(ResponseSize)) || !WriteExact(ClientHandle, ResponseUtf8.Get(), ResponseSize))
			{
				return;
			}
		}
	}

	FRunnableThread* Thread = nullptr;
	FString PipePath;
	HANDLE PipeHandle = INVALID_HANDLE_VALUE;
	std::atomic_bool bStopRequested = false;
};
#endif
} // namespace BlueprintBridge

class FBlueprintBridgeEditorModule final : public IModuleInterface
{
public:
	void StartupModule() override
	{
#if PLATFORM_WINDOWS
		PipeServer = MakeUnique<BlueprintBridge::FNamedPipeServer>();
		if (PipeServer->Start())
		{
			UE_LOG(LogBlueprintBridge, Display, TEXT("Blueprint Bridge listening on %s"), *BlueprintBridge::GetPipeNamePath());
		}
#endif
	}

	void ShutdownModule() override
	{
#if PLATFORM_WINDOWS
		PipeServer.Reset();
#endif
	}

private:
#if PLATFORM_WINDOWS
	TUniquePtr<BlueprintBridge::FNamedPipeServer> PipeServer;
#endif
};

IMPLEMENT_MODULE(FBlueprintBridgeEditorModule, BlueprintBridgeEditor)
