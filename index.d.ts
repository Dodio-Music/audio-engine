export interface AudioPlayerState {
    songFramesPlayed: number;
    songSecondsPlayed: number;
    ended: boolean;
    playing: boolean;
}

export class AudioPlayer {
    constructor();
    load(filePath: string): void;
    prepareNext(filePath: string): void;
    pause(): void;
    resume(): void;
    togglePlayPause(): boolean;
    seek(timeSeconds: number): void;
    setVolume(volume: number): void;
    onEnded(cb: () => void): void;
    onAdvanced(cb: (trackIndex: number) => void): void;

    getState(): AudioPlayerState;
}

declare const _default: {
    AudioPlayer: typeof AudioPlayer;
};

export default _default;
