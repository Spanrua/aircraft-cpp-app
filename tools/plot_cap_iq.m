% plot_cap_iq.m
% Read and visualize CAP files written by the Qt/C++ USRP capture path.
%
% Usage:
%   1. Run this script directly. It will open the latest CAP under
%      D:\cqt_platform\aircraft_project\data\raw when available.
%   2. Or set capPath before running:
%      capPath = "D:\cqt_platform\aircraft_project\data\raw\your_file.cap";
%      plot_cap_iq

clearvars -except capPath
close all
clc

headerLength = 512;
maxSamplesToLoad = 5e6;      % Limit memory for long captures.
timePlotSamples = 20000;
scatterPlotSamples = 50000;

if ~exist("capPath", "var") || strlength(string(capPath)) == 0
    scriptDir = fileparts(mfilename("fullpath"));
    rawDir = fullfile(scriptDir, "..", "..", "data", "raw");
    files = dir(fullfile(rawDir, "*.cap"));
    if isempty(files)
        [name, folder] = uigetfile("*.cap", "Select CAP file");
        if isequal(name, 0)
            error("No CAP file selected.");
        end
        capPath = fullfile(folder, name);
    else
        [~, idx] = max([files.datenum]);
        capPath = fullfile(files(idx).folder, files(idx).name);
    end
end

capPath = char(capPath);
fprintf("CAP file: %s\n", capPath);

fid = fopen(capPath, "rb", "ieee-be");
if fid < 0
    error("Cannot open CAP file: %s", capPath);
end
cleanup = onCleanup(@() fclose(fid));

header = fread(fid, headerLength, "uint8=>uint8");
if numel(header) < headerLength
    error("CAP file is shorter than the 512-byte header.");
end

version = char(header(1:min(8, numel(header)))');
version = string(regexprep(version, char(0), ""));
bandwidthHz = readDoubleBE(header, hex2dec("10"));
centerFrequencyHz = readDoubleBE(header, hex2dec("18"));
headerSampleCount = readUint32BE(header, hex2dec("110"));
blockSize = readUint32BE(header, hex2dec("114"));

fileInfo = dir(capPath);
payloadBytes = fileInfo.bytes - headerLength;
fileSampleCount = floor(payloadBytes / 4);
sampleCount = min([double(fileSampleCount), double(headerSampleCount), maxSamplesToLoad]);
if sampleCount <= 0
    error("CAP file has no IQ payload.");
end

raw = fread(fid, sampleCount * 2, "int16=>double");
if numel(raw) < sampleCount * 2
    sampleCount = floor(numel(raw) / 2);
    raw = raw(1:sampleCount * 2);
end

iData = raw(1:2:end);
qData = raw(2:2:end);
iq = complex(iData, qData) / 32768;

% The current CAP header stores bandwidth. The Qt app uses sample rate
% 1.28x bandwidth for preview compatibility.
sampleRateHz = bandwidthHz * 1.28;
if ~isfinite(sampleRateHz) || sampleRateHz <= 0
    sampleRateHz = 12.8e6;
end

dc = mean(iq);
rmsValue = sqrt(mean(abs(iq).^2));
peakValue = max(abs(iq));
clippedCount = nnz(abs(iData) >= 32760 | abs(qData) >= 32760);
clipRatio = clippedCount / sampleCount;

fprintf("Version: %s\n", version);
fprintf("File samples: %d, header samples: %d, loaded: %d\n", fileSampleCount, headerSampleCount, sampleCount);
fprintf("Center frequency: %.6f MHz\n", centerFrequencyHz / 1e6);
fprintf("Bandwidth: %.6f MHz\n", bandwidthHz / 1e6);
fprintf("Estimated sample rate: %.6f MSps\n", sampleRateHz / 1e6);
fprintf("Block size: %d\n", blockSize);
fprintf("Mean I/Q: %.4f%+.4fj, RMS: %.4f, Peak: %.4f, clipped: %.5f%%\n", ...
    real(dc), imag(dc), rmsValue, peakValue, clipRatio * 100);

figure("Name", "CAP IQ overview", "Color", "w");
tiledlayout(2, 2, "TileSpacing", "compact", "Padding", "compact");

nTime = min(timePlotSamples, sampleCount);
tMs = (0:nTime-1) / sampleRateHz * 1e3;
nexttile
plot(tMs, real(iq(1:nTime)), "b");
hold on
plot(tMs, imag(iq(1:nTime)), "r");
grid on
xlabel("Time (ms)");
ylabel("Normalized amplitude");
title("Time domain I/Q");
legend("I", "Q");

nexttile
nScatter = min(scatterPlotSamples, sampleCount);
step = max(1, floor(sampleCount / nScatter));
idx = 1:step:sampleCount;
idx = idx(1:min(numel(idx), nScatter));
plot(real(iq(idx)), imag(iq(idx)), ".", "MarkerSize", 3);
axis equal
grid on
xlabel("I");
ylabel("Q");
title("I/Q constellation");

nexttile
nfft = 4096;
if exist("pwelch", "file") == 2
    [pxx, f] = pwelch(iq, hamming(nfft), round(nfft * 0.75), nfft, sampleRateHz, "centered");
else
    nSpec = min(numel(iq), nfft * 64);
    windowed = iq(1:nSpec) .* hann(nSpec);
    pxx = abs(fftshift(fft(windowed, nfft))).^2 / nSpec;
    f = linspace(-sampleRateHz / 2, sampleRateHz / 2, nfft).';
end
plot((f + centerFrequencyHz) / 1e6, 10 * log10(pxx + eps));
grid on
xlabel("Frequency (MHz)");
ylabel("Power/Frequency (dB)");
title("Power spectrum");

nexttile
histogram(abs(iq), 100);
grid on
xlabel("Magnitude");
ylabel("Count");
title("Magnitude distribution");

figure("Name", "CAP IQ spectrogram", "Color", "w");
if exist("spectrogram", "file") == 2
    spectrogram(iq, hamming(1024), 768, 2048, sampleRateHz, "centered", "yaxis");
    title("Spectrogram");
else
    waterfallFallback(iq, sampleRateHz);
end

fprintf("\nChecks:\n");
fprintf("- Real RF/noise capture should not be all zero and should have nonzero RMS.\n");
fprintf("- Strong DC offset appears as high mean I/Q or center spike in spectrum.\n");
fprintf("- Clipping ratio near zero is expected; high clipping means gain/input is too strong.\n");

function value = readDoubleBE(header, zeroBasedOffset)
    bytes = uint8(header(zeroBasedOffset + (1:8)));
    value = typecast(swapbytes(typecast(bytes, "uint64")), "double");
end

function value = readUint32BE(header, zeroBasedOffset)
    bytes = uint8(header(zeroBasedOffset + (1:4)));
    value = double(typecast(swapbytes(typecast(bytes, "uint32")), "uint32"));
end

function waterfallFallback(iq, sampleRateHz)
    nfft = 1024;
    hop = 256;
    frameCount = floor((numel(iq) - nfft) / hop) + 1;
    frameCount = min(frameCount, 400);
    if frameCount <= 0
        plot(real(iq));
        title("Too few samples for spectrogram");
        return
    end
    spec = zeros(nfft, frameCount);
    win = hann(nfft);
    for k = 1:frameCount
        first = (k - 1) * hop + 1;
        frame = iq(first:first + nfft - 1) .* win;
        spec(:, k) = 20 * log10(abs(fftshift(fft(frame))) + eps);
    end
    fMHz = linspace(-sampleRateHz / 2, sampleRateHz / 2, nfft) / 1e6;
    tMs = ((0:frameCount - 1) * hop / sampleRateHz) * 1e3;
    imagesc(tMs, fMHz, spec);
    axis xy
    colorbar
    xlabel("Time (ms)");
    ylabel("Offset frequency (MHz)");
    title("Spectrogram fallback");
end
