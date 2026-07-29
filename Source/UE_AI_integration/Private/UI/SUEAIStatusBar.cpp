#include "UI/SUEAIStatusBar.h"

#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ISettingsModule.h"
#include "Infrastructure/ClientActivityService.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationEditorSettings.h"
#include "UEAIIntegrationSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UEAIIntegrationStatus"

namespace
{
using UEAIIntegration::Infrastructure::FClientActivityService;

UUEAIIntegrationSubsystem* GetIntegrationSubsystem()
{
	return GEditor
		? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
		: nullptr;
}

FString JsonString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Fallback = FString())
{
	FString Value;
	return Object.IsValid() && Object->TryGetStringField(Field, Value)
		? Value
		: Fallback;
}

double JsonNumber(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field)
{
	double Value = 0.0;
	if (Object.IsValid())
	{
		Object->TryGetNumberField(Field, Value);
	}
	return Value;
}

FString FormatDuration(const double DurationMs)
{
	if (DurationMs < 1000.0)
	{
		return FString::Printf(TEXT("%.0f ms"), DurationMs);
	}
	const double Seconds = DurationMs / 1000.0;
	if (Seconds < 60.0)
	{
		return FString::Printf(TEXT("%.1f s"), Seconds);
	}
	const int32 TotalSeconds = FMath::RoundToInt(Seconds);
	return FString::Printf(
		TEXT("%dm %02ds"),
		TotalSeconds / 60,
		TotalSeconds % 60);
}

FUIAction ReadOnlyAction()
{
	return FUIAction(
		FExecuteAction(),
		FCanExecuteAction::CreateLambda([] { return false; }));
}

void AddReadOnlyEntry(
	FMenuBuilder& MenuBuilder,
	const FText& Label,
	const FText& ToolTip = FText::GetEmpty(),
	const FSlateIcon& Icon = FSlateIcon())
{
	MenuBuilder.AddMenuEntry(
		Label,
		ToolTip,
		Icon,
		ReadOnlyAction());
}

FString GetState(const UUEAIIntegrationSubsystem* Subsystem)
{
	if (!Subsystem || !Subsystem->IsServerEnableRequested())
	{
		return TEXT("Disabled");
	}
	if (!Subsystem->IsServerEnabled())
	{
		return TEXT("Failed");
	}
	const FMCPToolRegistry* Registry = Subsystem->GetRegistry();
	return Registry && Registry->IsReady()
		? TEXT("Ready")
		: TEXT("Degraded");
}

FSlateIcon GetStateIcon(const FString& State)
{
	const FName Brush =
		State == TEXT("Disabled") ? TEXT("Icons.Unlink")
		: State == TEXT("Degraded") ? TEXT("Icons.WarningWithColor")
		: State == TEXT("Failed") ? TEXT("Icons.ErrorWithColor")
		: TEXT("Icons.Link");
	return FSlateIcon(FAppStyle::GetAppStyleSetName(), Brush);
}

void BuildMcpDetails(
	FMenuBuilder& MenuBuilder,
	const TSharedPtr<FJsonObject> Client)
{
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ClientVersion", "Version: {0}"),
			FText::FromString(JsonString(
				Client,
				TEXT("version"),
				TEXT("Unknown")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ClientPid", "PID: {0}"),
			FText::AsNumber(JsonNumber(Client, TEXT("pid")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ClientTransport", "Transport: {0}"),
			FText::FromString(JsonString(
				Client,
				TEXT("transport"),
				TEXT("Unknown")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ClientOnline", "Online: {0}"),
			FText::FromString(FormatDuration(
				JsonNumber(Client, TEXT("onlineForSeconds")) * 1000.0))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ClientActivity", "Last activity: {0}"),
			FText::FromString(JsonString(
				Client,
				TEXT("lastActivityAtUtc"),
				TEXT("Unknown")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ClientCalls", "Calls: {0}"),
			FText::AsNumber(JsonNumber(Client, TEXT("callCount")))));
}

void BuildCliDetails(
	FMenuBuilder& MenuBuilder,
	const TSharedPtr<FJsonObject> Invocation)
{
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("CliVersion", "Version: {0}"),
			FText::FromString(JsonString(
				Invocation,
				TEXT("version"),
				TEXT("Unknown")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("CliPid", "PID: {0}"),
			FText::AsNumber(JsonNumber(Invocation, TEXT("pid")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("CliStatus", "Status: {0}"),
			FText::FromString(JsonString(
				Invocation,
				TEXT("status"),
				TEXT("Unknown")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("CliDuration", "Duration: {0}"),
			FText::FromString(FormatDuration(
				JsonNumber(Invocation, TEXT("durationMs"))))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("CliInvocation", "Invocation: {0}"),
			FText::FromString(JsonString(
				Invocation,
				TEXT("invocationId"),
				TEXT("Unknown")))));
}

void BuildExecutionDetails(
	FMenuBuilder& MenuBuilder,
	const TSharedPtr<FJsonObject> Activity)
{
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ExecutionCaller", "Caller: {0}"),
			FText::FromString(JsonString(
				Activity,
				TEXT("callerName"),
				TEXT("Legacy HTTP")))));
	const FString Subject =
		!JsonString(Activity, TEXT("capability")).IsEmpty()
			? JsonString(Activity, TEXT("capability"))
			: JsonString(Activity, TEXT("workflowAction"), TEXT("Unknown"));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ExecutionAction", "Action: {0}"),
			FText::FromString(Subject)));
	const FString RequestId = JsonString(Activity, TEXT("requestId"));
	const FString RunId = JsonString(Activity, TEXT("runId"));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ExecutionCorrelation", "Request / Run: {0} / {1}"),
			FText::FromString(RequestId.IsEmpty() ? TEXT("-") : RequestId),
			FText::FromString(RunId.IsEmpty() ? TEXT("-") : RunId)));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ExecutionRisk", "Risk: {0}"),
			FText::FromString(JsonString(
				Activity,
				TEXT("risk"),
				TEXT("Unknown")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ExecutionResult", "Result: {0} (HTTP {1})"),
			FText::FromString(JsonString(
				Activity,
				TEXT("status"),
				TEXT("Unknown"))),
			FText::AsNumber(JsonNumber(Activity, TEXT("httpStatus")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ExecutionDuration", "Duration: {0}"),
			FText::FromString(FormatDuration(
				JsonNumber(Activity, TEXT("durationMs"))))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("ExecutionError", "Error code: {0}"),
			FText::FromString(JsonString(
				Activity,
				TEXT("errorCode"),
				TEXT("-")))));
}

void BuildStatistics(
	FMenuBuilder& MenuBuilder,
	const TSharedPtr<FJsonObject> Stats)
{
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT(
				"CapabilityStats",
				"Capability: {0} calls, {1} succeeded, {2} failed"),
			FText::AsNumber(JsonNumber(Stats, TEXT("capabilityCalls"))),
			FText::AsNumber(JsonNumber(Stats, TEXT("capabilitySucceeded"))),
			FText::AsNumber(JsonNumber(Stats, TEXT("capabilityFailed")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT(
				"DslStats",
				"DSL Run: {0} runs, {1} completed, {2} failed, {3} blocked"),
			FText::AsNumber(JsonNumber(Stats, TEXT("dslRuns"))),
			FText::AsNumber(JsonNumber(Stats, TEXT("dslCompleted"))),
			FText::AsNumber(JsonNumber(Stats, TEXT("dslFailed"))),
			FText::AsNumber(JsonNumber(Stats, TEXT("dslBlocked")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("RollbackStats", "Rollback: {0}"),
			FText::AsNumber(JsonNumber(Stats, TEXT("rollbacks")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT(
				"OperationStats",
				"Operations: {0} succeeded / {1} total"),
			FText::AsNumber(JsonNumber(Stats, TEXT("operationSucceeded"))),
			FText::AsNumber(JsonNumber(Stats, TEXT("operationTotal")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT("CliStats", "CLI invocations: {0}"),
			FText::AsNumber(JsonNumber(Stats, TEXT("cliInvocations")))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT(
				"CapabilityLatency",
				"Capability latency p50 / p95: {0} / {1}"),
			FText::FromString(FormatDuration(
				JsonNumber(Stats, TEXT("capabilityP50Ms")))),
			FText::FromString(FormatDuration(
				JsonNumber(Stats, TEXT("capabilityP95Ms"))))));
	AddReadOnlyEntry(
		MenuBuilder,
		FText::Format(
			LOCTEXT(
				"DslLatency",
				"DSL Run latency p50 / p95: {0} / {1}"),
			FText::FromString(FormatDuration(
				JsonNumber(Stats, TEXT("dslRunP50Ms")))),
			FText::FromString(FormatDuration(
				JsonNumber(Stats, TEXT("dslRunP95Ms"))))));
}
}

void SUEAIStatusBarWidget::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SComboButton)
		.ComboButtonStyle(
			&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>(
				TEXT("SimpleComboButton")))
		.ButtonStyle(
			&FAppStyle::Get().GetWidgetStyle<FButtonStyle>(
				TEXT("StatusBar.StatusBarButton")))
		.ContentPadding(FMargin(4.0f, 0.0f))
		.MenuPlacement(MenuPlacement_AboveAnchor)
		.OnGetMenuContent(this, &SUEAIStatusBarWidget::MakeMenuContent)
		.HasDownArrow(false)
		.IsFocusable(false)
		.ToolTipText(this, &SUEAIStatusBarWidget::GetStatusToolTip)
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(this, &SUEAIStatusBarWidget::GetStatusIcon)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(3.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SUEAIStatusBarWidget::GetStatusCountText)
				.Visibility(
					this,
					&SUEAIStatusBarWidget::GetStatusCountVisibility)
			]
		]
	];
}

TSharedRef<SWidget> SUEAIStatusBarWidget::MakeMenuContent()
{
	UUEAIIntegrationSubsystem* Subsystem = GetIntegrationSubsystem();
	const TWeakObjectPtr<UUEAIIntegrationSubsystem> WeakSubsystem(Subsystem);
	const FString State = GetState(Subsystem);
	TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	if (Subsystem)
	{
		if (FClientActivityService* Service =
			Subsystem->GetClientActivityService())
		{
			Snapshot = Service->MakeSnapshot(10, 5);
		}
	}

	FMenuBuilder MenuBuilder(true, nullptr);
	MenuBuilder.BeginSection(
		TEXT("UEAIStatus"),
		FText::Format(
			LOCTEXT("MenuTitle", "UE AI Integration    {0}"),
			FText::FromString(State)));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("EnableServer", "Enable Server"),
		LOCTEXT(
			"EnableServerTooltip",
			"Enable or disable the loopback UE AI HTTP service."),
		GetStateIcon(State),
		FUIAction(
			FExecuteAction::CreateLambda(
				[WeakSubsystem]
				{
					if (UUEAIIntegrationSubsystem* PinnedSubsystem =
						WeakSubsystem.Get())
					{
						PinnedSubsystem->SetServerEnabled(
							!PinnedSubsystem->IsServerEnableRequested());
					}
				}),
			FCanExecuteAction::CreateLambda(
				[WeakSubsystem]
				{
					return WeakSubsystem.Get() != nullptr;
				}),
			FIsActionChecked::CreateLambda(
				[WeakSubsystem]
				{
					if (const UUEAIIntegrationSubsystem* PinnedSubsystem =
						WeakSubsystem.Get())
					{
						return PinnedSubsystem->IsServerEnableRequested();
					}
					return false;
				})),
		NAME_None,
		EUserInterfaceActionType::ToggleButton);
	AddReadOnlyEntry(
		MenuBuilder,
		FText::FromString(
			Subsystem
				? FString::Printf(
					TEXT("127.0.0.1:%d"),
					Subsystem->GetConfiguredPort())
				: TEXT("Unavailable")));
	MenuBuilder.EndSection();

	const TArray<TSharedPtr<FJsonValue>> McpClients =
		Snapshot->HasTypedField<EJson::Array>(TEXT("mcpClients"))
		? Snapshot->GetArrayField(TEXT("mcpClients"))
		: TArray<TSharedPtr<FJsonValue>>();
	MenuBuilder.AddSubMenu(
		FText::Format(
			LOCTEXT("McpClients", "MCP Clients ({0})"),
			FText::AsNumber(McpClients.Num())),
		LOCTEXT("McpClientsTooltip", "Online MCP client connections."),
		FNewMenuDelegate::CreateLambda(
			[McpClients](FMenuBuilder& SubMenu)
			{
				if (McpClients.IsEmpty())
				{
					AddReadOnlyEntry(
						SubMenu,
						LOCTEXT("NoMcpClients", "No online MCP clients"));
					return;
				}
				for (const TSharedPtr<FJsonValue>& Value : McpClients)
				{
					const TSharedPtr<FJsonObject> Client =
						Value->AsObject();
					SubMenu.AddSubMenu(
						FText::FromString(JsonString(
							Client,
							TEXT("name"),
							TEXT("Unknown MCP"))),
						FText::GetEmpty(),
						FNewMenuDelegate::CreateLambda(
							[Client](FMenuBuilder& Details)
							{
								BuildMcpDetails(Details, Client);
							}));
				}
			}));

	const TArray<TSharedPtr<FJsonValue>> CliInvocations =
		Snapshot->HasTypedField<EJson::Array>(TEXT("cliInvocations"))
		? Snapshot->GetArrayField(TEXT("cliInvocations"))
		: TArray<TSharedPtr<FJsonValue>>();
	const int32 RunningCli = static_cast<int32>(
		JsonNumber(Snapshot, TEXT("runningCliCount")));
	const int32 RecentCli =
		FMath::Max(0, CliInvocations.Num() - RunningCli);
	MenuBuilder.AddSubMenu(
		FText::Format(
			LOCTEXT("CliMenu", "CLI ({0} running, {1} recent)"),
			FText::AsNumber(RunningCli),
			FText::AsNumber(RecentCli)),
		LOCTEXT("CliTooltip", "Running and five most recent CLI invocations."),
		FNewMenuDelegate::CreateLambda(
			[CliInvocations](FMenuBuilder& SubMenu)
			{
				if (CliInvocations.IsEmpty())
				{
					AddReadOnlyEntry(
						SubMenu,
						LOCTEXT("NoCli", "No CLI invocations"));
					return;
				}
				for (const TSharedPtr<FJsonValue>& Value : CliInvocations)
				{
					const TSharedPtr<FJsonObject> Invocation =
						Value->AsObject();
					const FString Name = JsonString(
						Invocation,
						TEXT("name"),
						TEXT("ue"));
					const FString Command = JsonString(
						Invocation,
						TEXT("command"));
					const FString Status = JsonString(
						Invocation,
						TEXT("status"));
					SubMenu.AddSubMenu(
						FText::FromString(FString::Printf(
							TEXT("%s %s    %s"),
							*Name,
							*Command,
							*Status)),
						FText::GetEmpty(),
						FNewMenuDelegate::CreateLambda(
							[Invocation](FMenuBuilder& Details)
							{
								BuildCliDetails(Details, Invocation);
							}));
				}
			}));

	const TArray<TSharedPtr<FJsonValue>> Executions =
		Snapshot->HasTypedField<EJson::Array>(TEXT("recentExecutions"))
		? Snapshot->GetArrayField(TEXT("recentExecutions"))
		: TArray<TSharedPtr<FJsonValue>>();
	MenuBuilder.AddSubMenu(
		FText::Format(
			LOCTEXT("RecentExecutions", "Recent Executions ({0})"),
			FText::AsNumber(Executions.Num())),
		LOCTEXT(
			"RecentExecutionsTooltip",
			"Ten most recent capability and Workflow API executions."),
		FNewMenuDelegate::CreateLambda(
			[Executions](FMenuBuilder& SubMenu)
			{
				if (Executions.IsEmpty())
				{
					AddReadOnlyEntry(
						SubMenu,
						LOCTEXT("NoExecutions", "No executions recorded"));
					return;
				}
				for (const TSharedPtr<FJsonValue>& Value : Executions)
				{
					const TSharedPtr<FJsonObject> Activity =
						Value->AsObject();
					FString Subject = JsonString(
						Activity,
						TEXT("capability"));
					if (Subject.IsEmpty())
					{
						Subject = JsonString(
							Activity,
							TEXT("workflowAction"),
							TEXT("Workflow"));
					}
					const FString Status = JsonString(
						Activity,
						TEXT("status"));
					SubMenu.AddSubMenu(
						FText::FromString(
							Subject + TEXT("    ") + Status),
						FText::GetEmpty(),
						FNewMenuDelegate::CreateLambda(
							[Activity](FMenuBuilder& Details)
							{
								BuildExecutionDetails(Details, Activity);
							}));
				}
			}));

	const TSharedPtr<FJsonObject>* StatsPtr = nullptr;
	const TSharedPtr<FJsonObject> Stats =
		Snapshot->TryGetObjectField(TEXT("statistics"), StatsPtr)
			&& StatsPtr && StatsPtr->IsValid()
		? *StatsPtr
		: MakeShared<FJsonObject>();
	MenuBuilder.AddSubMenu(
		LOCTEXT("SessionStatistics", "Session Statistics"),
		LOCTEXT(
			"SessionStatisticsTooltip",
			"Cumulative counters for this Editor session."),
		FNewMenuDelegate::CreateLambda(
			[Stats](FMenuBuilder& SubMenu)
			{
				BuildStatistics(SubMenu, Stats);
			}));

	MenuBuilder.BeginSection(TEXT("UEAISettings"));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("Settings", "Settings..."),
		LOCTEXT(
			"SettingsTooltip",
			"Open UE AI Integration Editor preferences."),
		FSlateIcon(
			FAppStyle::GetAppStyleSetName(),
			TEXT("Icons.Settings")),
		FUIAction(FExecuteAction::CreateLambda(
			[]
			{
				const UUEAIIntegrationEditorSettings* Settings =
					GetDefault<UUEAIIntegrationEditorSettings>();
				if (ISettingsModule* SettingsModule =
					FModuleManager::LoadModulePtr<ISettingsModule>(
						TEXT("Settings")))
				{
					SettingsModule->ShowViewer(
						Settings->GetContainerName(),
						Settings->GetCategoryName(),
						Settings->GetSectionName());
				}
			})));
	MenuBuilder.EndSection();
	return MenuBuilder.MakeWidget();
}

FText SUEAIStatusBarWidget::GetStatusCountText() const
{
	const UUEAIIntegrationSubsystem* Subsystem = GetIntegrationSubsystem();
	const FClientActivityService* Service =
		Subsystem ? Subsystem->GetClientActivityService() : nullptr;
	return FText::AsNumber(Service ? Service->GetOnlineMcpCount() : 0);
}

EVisibility SUEAIStatusBarWidget::GetStatusCountVisibility() const
{
	const UUEAIIntegrationSubsystem* Subsystem = GetIntegrationSubsystem();
	const FClientActivityService* Service =
		Subsystem ? Subsystem->GetClientActivityService() : nullptr;
	return GetState(Subsystem) == TEXT("Ready")
		&& Service && Service->GetOnlineMcpCount() > 0
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText SUEAIStatusBarWidget::GetStatusToolTip() const
{
	const UUEAIIntegrationSubsystem* Subsystem = GetIntegrationSubsystem();
	if (!Subsystem)
	{
		return LOCTEXT(
			"TooltipUnavailable",
			"UE AI Integration subsystem is unavailable.");
	}
	const FClientActivityService* Service =
		Subsystem->GetClientActivityService();
	return FText::Format(
		LOCTEXT(
			"Tooltip",
			"State: {0}\n"
			"Listen: 127.0.0.1:{1}\n"
			"Online MCP: {2}\n"
			"Running CLI: {3}\n"
			"Last execution: {4}"),
		FText::FromString(GetState(Subsystem)),
		FText::AsNumber(Subsystem->GetConfiguredPort()),
		FText::AsNumber(Service ? Service->GetOnlineMcpCount() : 0),
		FText::AsNumber(Service ? Service->GetRunningCliCount() : 0),
		FText::FromString(
			Service ? Service->GetLastExecutionResult() : TEXT("None")));
}

const FSlateBrush* SUEAIStatusBarWidget::GetStatusIcon() const
{
	const FString State = GetState(GetIntegrationSubsystem());
	const FName Brush =
		State == TEXT("Disabled") ? TEXT("Icons.Unlink")
		: State == TEXT("Degraded") ? TEXT("Icons.WarningWithColor")
		: State == TEXT("Failed") ? TEXT("Icons.ErrorWithColor")
		: TEXT("Icons.Link");
	return FAppStyle::GetBrush(Brush);
}

TSharedRef<SWidget> CreateUEAIStatusBarWidget()
{
	return SNew(SUEAIStatusBarWidget);
}

#undef LOCTEXT_NAMESPACE
