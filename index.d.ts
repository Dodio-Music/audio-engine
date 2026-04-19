import { Buffer } from "node:buffer";

export interface AudioPlayerState {
    sampleRate: number;
    channels: number;
    bufferedFrames: number;
    bufferCapacityFrames: number;
    underrunCount: number;
    eos: boolean;
    transportState: "stopped" | "buffering" | "playing" | "paused";
    volume: boolean;
}

export class AudioPlayer {
    constructor();
    initDevice({bufferCapacityMs, drainLowWaterMs, startThresholdMs}?: {
        bufferCapacityMs: number,
        drainLowWaterMs: number,
        startThresholdMs: number
    }): boolean;
    play(): void;
    stop(): void;
    write(buffer: Buffer): number;
    getState(): AudioPlayerState;
    pause(): void;
    flush(): void;
    endOfStream(): void;
    setDrainCallback(callback: () => void): void;
    setEndedCallback(callback: () => void): void;
    setVolume(volume: number): void;
}

declare const _default: {
    AudioPlayer: typeof AudioPlayer;
};

export default _default;
