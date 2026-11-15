using System;
using System.IO;
using System.Net.Http;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Windows.Data.Json;
using Windows.Security.Cryptography;
using Windows.Security.Cryptography.Core;

namespace Cexbox
{
    // Downloads the official Windows build of Claude Code from Anthropic and checks it
    // against the checksum in Anthropic's release manifest, the same way install.ps1 does.
    static class ClaudeDownloader
    {
        const string BaseUrl = "https://downloads.claude.ai/claude-code-releases";
        const string Platform = "win32-x64";

        public static async Task<string> DownloadAsync(string dir, Action<long, long> progress)
        {
            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) })
            {
                string version = (await http.GetStringAsync(BaseUrl + "/latest")).Trim();
                if (!Regex.IsMatch(version, @"^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$"))
                {
                    throw new InvalidOperationException("The download server didn't return a version number. Check the Xbox's internet connection.");
                }

                JsonObject manifest = JsonObject.Parse(await http.GetStringAsync($"{BaseUrl}/{version}/manifest.json"));
                JsonObject entry = manifest.GetNamedObject("platforms").GetNamedObject(Platform);
                string expected = entry.GetNamedString("checksum").ToLowerInvariant();
                long size = (long)entry.GetNamedNumber("size");

                Directory.CreateDirectory(dir);
                string partial = Path.Combine(dir, "claude.exe.part");
                string target = Path.Combine(dir, "claude.exe");
                CryptographicHash hash = HashAlgorithmProvider.OpenAlgorithm(HashAlgorithmNames.Sha256).CreateHash();

                using (HttpResponseMessage response = await http.GetAsync($"{BaseUrl}/{version}/{Platform}/claude.exe", HttpCompletionOption.ResponseHeadersRead))
                {
                    response.EnsureSuccessStatusCode();
                    using (Stream source = await response.Content.ReadAsStreamAsync())
                    using (var file = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
                    {
                        var buffer = new byte[1 << 20];
                        long done = 0;
                        int n;
                        while ((n = await source.ReadAsync(buffer, 0, buffer.Length)) > 0)
                        {
                            await file.WriteAsync(buffer, 0, n);
                            var chunk = new byte[n];
                            Buffer.BlockCopy(buffer, 0, chunk, 0, n);
                            hash.Append(CryptographicBuffer.CreateFromByteArray(chunk));
                            done += n;
                            progress(done, size);
                        }
                    }
                }

                string actual = CryptographicBuffer.EncodeToHexString(hash.GetValueAndReset()).ToLowerInvariant();
                if (actual != expected)
                {
                    File.Delete(partial);
                    throw new InvalidOperationException("The download didn't match Anthropic's checksum, so it was deleted. Try again.");
                }

                if (File.Exists(target)) File.Delete(target);
                File.Move(partial, target);
                File.WriteAllText(Path.Combine(dir, "version.txt"), version);
                return version;
            }
        }

        const string BusyboxUrl = "https://frippery.org/files/busybox/";
        const string BusyboxFile = "busybox64u.exe";

        // Downloads busybox-w32 (a shell plus Unix tools in one program) and saves it as
        // bash.exe, which Claude Code uses for its Bash tool. Checked against the
        // checksum file published next to it.
        public static async Task DownloadBusyboxAsync(string binDir)
        {
            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) })
            {
                string sums = await http.GetStringAsync(BusyboxUrl + "SHA256SUM");
                string expected = null;
                foreach (string line in sums.Split('\n'))
                {
                    string[] parts = line.Trim().Split(new[] { ' ', '*' }, StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length == 2 && parts[1] == BusyboxFile) expected = parts[0].ToLowerInvariant();
                }
                if (expected == null) throw new InvalidOperationException("busybox's checksum file doesn't list " + BusyboxFile + ".");

                byte[] data = await http.GetByteArrayAsync(BusyboxUrl + BusyboxFile);
                CryptographicHash hash = HashAlgorithmProvider.OpenAlgorithm(HashAlgorithmNames.Sha256).CreateHash();
                hash.Append(CryptographicBuffer.CreateFromByteArray(data));
                string actual = CryptographicBuffer.EncodeToHexString(hash.GetValueAndReset()).ToLowerInvariant();
                if (actual != expected) throw new InvalidOperationException("The busybox download didn't match its checksum.");

                Directory.CreateDirectory(binDir);
                File.WriteAllBytes(Path.Combine(binDir, "bash.exe"), data);
            }
        }
    }
}
