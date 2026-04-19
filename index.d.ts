import { Buffer } from "node:buffer";

export interface AudioPlayerState {
    sampleRate: number;
    channels: number;
    bufferedFrames: number;
    bufferCapacityFrames: number;
    underrunCount: number;
    eos: boolean;
    transportState: "stopped" | "playing" | "paused";
}

export class AudioPlayer {
    constructor();
    initDevice(): boolean;
    play(): void;
    stop(): void;
    write(buffer: Buffer): number;
    getState(): AudioPlayerState;
    pause(): void;
    flush(): void;
    endOfStream(): void;
    setDrainCallback(callback: () => void): void;
    setEndedCallback(callback: () => void): void;
}

declare const _default: {
    AudioPlayer: typeof AudioPlayer;
};

export default _default;
