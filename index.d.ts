export interface AudioPlayerState {
    songFramesPlayed: number;
    songSecondsPlayed: number;
    ended: boolean;
}

export class AudioPlayer {
    constructor();
    load(filePath: string): void;
    pause(): void;
    resume(): void;
    togglePlayPause(): boolean;
    seek(timeSeconds: number): void;
    setVolume(volume: number): void;

    getState(): AudioPlayerState;
}

declare const _default: {
    AudioPlayer: typeof AudioPlayer;
};

export default _default;
