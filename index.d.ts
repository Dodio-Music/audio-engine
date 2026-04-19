import {Buffer} from "node:buffer";

export interface AudioPlayerState {
    sampleRate: number;
    channels: number;
    bufferedFrames: number;
    underrunCount: number;
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
}

declare const _default: {
    AudioPlayer: typeof AudioPlayer;
};

export default _default;
