import { EmulationService } from './emulation.service';
import { Injectable } from '@angular/core';
import { Mutex } from 'async-mutex';
import { PwmUpdate } from './../helper/Cloudpilot';

type AudioContextType = typeof AudioContext;

declare global {
    interface Window {
        webkitAudioContext: AudioContextType;
        AudioContext: AudioContextType;
    }
}

const audioContextCtor = window.AudioContext || window.webkitAudioContext;

const INTERACTION_EVENTS = ['touchstart', 'click', 'keydown'];

function withTimeout<T>(v: Promise<T>, timeout = 100): Promise<T> {
    return new Promise((resolve, reject) => {
        setTimeout(() => reject(new Error('timeout')), timeout);

        v.then(resolve, reject);
    });
}

@Injectable({
    providedIn: 'root',
})
export class AudioService {
    constructor(private emulationService: EmulationService) {
        this.emulationService.pwmUpdateEvent.addHandler(this.onPwmUpdate);
    }

    initialize = (): Promise<void> =>
        this.mutex.runExclusive(async () => {
            if (this.initialized) return;

            try {
                this.context = new audioContextCtor();
            } catch (e) {
                console.error(e);
                console.error('web audio not available');

                return;
            }

            try {
                this.context.destination.channelCount = 1;
            } catch (e) {
                console.warn('audio driver: failed to set channel count');
            }

            try {
                await this.start();
            } catch (e) {
                console.error(e);
                console.error('failed to initialize audio context');

                return;
            }

            this.gainNode = this.context.createGain();
            this.gainNode.channelCount = 1;
            this.gainNode.channelInterpretation = 'speakers';
            this.gainNode.gain.value = 0.25;

            this.gainNode.connect(this.context.destination);

            this.applyPwmUpdate();

            this.initialized = true;
        });

    isInitialized(): boolean {
        return this.initialized;
    }

    private async start(): Promise<void> {
        if (!this.context) return;

        await withTimeout(this.context.resume());
        // await this.context.suspend();

        this.suspended = true;
        console.log('audio context initialized');
    }

    private onPwmUpdate = async (pwmUpdate: PwmUpdate): Promise<void> => {
        this.pendingPwmUpdate = pwmUpdate;

        if (!this.initialized) return;

        this.applyPwmUpdate();
    };

    private applyPwmUpdate(): void {
        if (!this.context || !this.pendingPwmUpdate) return;

        if (this.bufferSourceNode) {
            this.bufferSourceNode.stop();
            this.bufferSourceNode.disconnect();
            this.bufferSourceNode = undefined;
        }

        const { frequency, dutyCycle } = this.pendingPwmUpdate;
        this.pendingPwmUpdate = undefined;

        if (frequency <= 0 && dutyCycle <= 0) {
            return;
        }

        const sampleRate = this.context.sampleRate;

        const buffer = this.context.createBuffer(1, Math.round(sampleRate / frequency), sampleRate);
        const data = buffer.getChannelData(0);

        for (let i = 0; i < data.length; i++) {
            data[i] = i / data.length < dutyCycle ? 1 : 0;
        }

        this.bufferSourceNode = this.context.createBufferSource();
        this.bufferSourceNode.channelCount = 1;
        this.bufferSourceNode.channelInterpretation = 'speakers';
        this.bufferSourceNode.loop = true;
        this.bufferSourceNode.buffer = null;
        this.bufferSourceNode.buffer = buffer;

        this.bufferSourceNode.connect(this.gainNode);
        this.bufferSourceNode.start();

        this.pendingPwmUpdate = undefined;
    }

    private mutex = new Mutex();
    private context: AudioContext | undefined;

    private bufferSourceNode: AudioBufferSourceNode | undefined;
    private gainNode!: GainNode;

    private suspended = true;
    private initialized = false;

    private pendingPwmUpdate: PwmUpdate | undefined;
}