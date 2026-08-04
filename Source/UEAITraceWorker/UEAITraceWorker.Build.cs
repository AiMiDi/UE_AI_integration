using UnrealBuildTool;
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public class UEAITraceWorker : ModuleRules
{
	private static string Sha256Files(string root, IEnumerable<string> relativeFiles)
	{
		var files = new List<string>(relativeFiles);
		files.Sort(StringComparer.Ordinal);
		using (var canonical = new MemoryStream())
		{
			foreach (string relative in files)
			{
				byte[] name = Encoding.UTF8.GetBytes(relative.Replace('\\', '/'));
				canonical.Write(name, 0, name.Length);
				canonical.WriteByte(0);
				byte[] content = File.ReadAllBytes(Path.Combine(root, relative));
				canonical.Write(content, 0, content.Length);
				canonical.WriteByte(0);
			}
			return "sha256:" + Sha256(canonical.ToArray());
		}
	}

	private static string Sha256File(string path)
	{
		return "sha256:" + Sha256(File.ReadAllBytes(path));
	}

	// ModuleRules assemblies intentionally have a small reference set in UE 5.3,
	// so keep this build-only SHA-256 implementation self-contained.
	private static string Sha256(byte[] input)
	{
		uint[] constants = new uint[]
		{
			0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
			0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
			0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
			0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
			0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
			0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
			0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
			0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
			0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
			0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
			0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
			0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
			0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
			0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
			0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
			0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
		};
		ulong bitLength = (ulong)input.LongLength * 8UL;
		int paddedLength = ((input.Length + 9 + 63) / 64) * 64;
		byte[] message = new byte[paddedLength];
		Buffer.BlockCopy(input, 0, message, 0, input.Length);
		message[input.Length] = 0x80;
		for (int index = 0; index < 8; ++index)
		{
			message[paddedLength - 1 - index] = (byte)(bitLength >> (8 * index));
		}
		uint[] state = new uint[]
		{
			0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
			0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
		};
		uint[] words = new uint[64];
		for (int offset = 0; offset < message.Length; offset += 64)
		{
			for (int index = 0; index < 16; ++index)
			{
				int at = offset + index * 4;
				words[index] = ((uint)message[at] << 24)
					| ((uint)message[at + 1] << 16)
					| ((uint)message[at + 2] << 8)
					| message[at + 3];
			}
			for (int index = 16; index < 64; ++index)
			{
				uint x = words[index - 15];
				uint y = words[index - 2];
				uint s0 = RotateRight(x, 7) ^ RotateRight(x, 18) ^ (x >> 3);
				uint s1 = RotateRight(y, 17) ^ RotateRight(y, 19) ^ (y >> 10);
				words[index] = unchecked(words[index - 16] + s0
					+ words[index - 7] + s1);
			}
			uint a=state[0],b=state[1],c=state[2],d=state[3];
			uint e=state[4],f=state[5],g=state[6],h=state[7];
			for (int index = 0; index < 64; ++index)
			{
				uint sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
				uint choose = (e & f) ^ ((~e) & g);
				uint temp1 = unchecked(h + sum1 + choose + constants[index] + words[index]);
				uint sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
				uint majority = (a & b) ^ (a & c) ^ (b & c);
				uint temp2 = unchecked(sum0 + majority);
				h=g; g=f; f=e; e=unchecked(d + temp1);
				d=c; c=b; b=a; a=unchecked(temp1 + temp2);
			}
			state[0]=unchecked(state[0]+a); state[1]=unchecked(state[1]+b);
			state[2]=unchecked(state[2]+c); state[3]=unchecked(state[3]+d);
			state[4]=unchecked(state[4]+e); state[5]=unchecked(state[5]+f);
			state[6]=unchecked(state[6]+g); state[7]=unchecked(state[7]+h);
		}
		var output = new StringBuilder(64);
		foreach (uint value in state)
		{
			output.Append(value.ToString("x8"));
		}
		return output.ToString();
	}

	private static uint RotateRight(uint value, int count)
	{
		return (value >> count) | (value << (32 - count));
	}

	private static string EscapeDefinition(string value)
	{
		return value.Replace("\\", "\\\\").Replace("\"", "\\\"");
	}

	public UEAITraceWorker(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivatePCHHeaderFile = "Private/UEAITraceWorkerPrivatePCH.h";
		bUseUnity = false;
		CppStandard = CppStandardVersion.Cpp20;
		PublicIncludePathModuleNames.Add("Launch");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"Core",
			"CoreUObject",
			"Json",
			"Projects",
			"SSL",
			"TraceServices",
			"UEAITraceAnalysisCore"
		});
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Advapi32.lib");
		}

		string pluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string engineVersion = String.Format(
			"{0}.{1}", Target.Version.MajorVersion, Target.Version.MinorVersion);
		string actionMapping = Path.Combine(
			pluginRoot, "Resources", "Trace", "insights-actions." + engineVersion + ".json");
		string launchProfiles = Path.Combine(
			pluginRoot, "Resources", "Trace", "launch-profiles.json");
		ExternalDependencies.Add(launchProfiles);
		if (!File.Exists(actionMapping))
		{
			throw new BuildException(
				"UEAITraceWorker has no Insights action mapping for Engine {0}.",
				engineVersion);
		}
		string contractDigest = Sha256Files(pluginRoot, new string[]
		{
			Path.Combine("Resources", "Capabilities", "production.json"),
			Path.Combine("Resources", "Trace", "insights-actions." + engineVersion + ".json"),
			Path.Combine("Resources", "Trace", "launch-profiles.json"),
			Path.Combine("Resources", "Trace", "worker-protocol.v1.json")
		});
		string providerDigest = Sha256File(actionMapping);
		string launchProfilesDigest = Sha256File(launchProfiles);

		PublicDefinitions.Add("UEAI_TRACE_WORKER_VERSION=\"0.9.0\"");
		PublicDefinitions.Add(
			"UEAI_TRACE_CONTRACT_DIGEST=\"" + contractDigest + "\"");
		PublicDefinitions.Add(
			"UEAI_TRACE_PROVIDER_SCHEMA_DIGEST=\"" + providerDigest + "\"");
		PublicDefinitions.Add(
			"UEAI_TRACE_LAUNCH_PROFILES_DIGEST=\"" + launchProfilesDigest + "\"");
		PublicDefinitions.Add(
			"UEAI_TRACE_SOURCE_ROOT=\"" + EscapeDefinition(pluginRoot) + "\"");
	}
}
