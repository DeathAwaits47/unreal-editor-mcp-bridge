// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MCP/MCPToolBase.h"

/**
 * Read-first performance and material-cost inspection for the active UE level.
 *
 * Scene audits report measurable editor data (instances, triangles, shadow
 * casters, lights, Nanite, and material use). Exact GPU/CPU frame time still
 * requires a PIE or packaged runtime profile.
 */
class FMCPTool_Performance : public FMCPToolBase
{
public:
	virtual ~FMCPTool_Performance() override;
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	struct FPIEPerfSample
	{
		double ElapsedSeconds = 0.0;
		double FrameMs = 0.0;
		double GameThreadMs = 0.0;
		double RenderThreadMs = 0.0;
		double GpuMs = 0.0;
	};

	FMCPToolResult ExecuteSceneAudit(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteMaterialAudit(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRuntimeProfileCommand(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecutePIECapture(const TSharedRef<FJsonObject>& Params);
	bool TickPIECapture(float DeltaSeconds);
	void StopPIECapture(bool bWriteReport);
	TSharedPtr<FJsonObject> BuildPIECaptureSummary() const;
	FString SavePIECaptureReport() const;

	FTSTicker::FDelegateHandle PIECaptureTickerHandle;
	TArray<FPIEPerfSample> PIECaptureSamples;
	double PIECaptureStartedAt = 0.0;
	double PIECaptureLastSampleAt = 0.0;
	double PIECaptureIntervalSeconds = 0.25;
	double PIECaptureMaxDurationSeconds = 180.0;
	FString LastPIECaptureReportPath;
};
