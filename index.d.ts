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
    songFramesPlayed: number;
    songFramesWritten: number;
}

export class AudioPlayer {
    constructor();
    load(filePath: string): void;
    pause(): void;
    resume(): void;
    togglePlayPause(): boolean;
    seek(timeSeconds: number): void;

    getState(): AudioPlayerState;
    setVolume(volume: number): void;
}

declare const _default: {
    AudioPlayer: typeof AudioPlayer;
};

export default _default;
