import { initializeApp } from 'firebase/app';
import { getDatabase } from 'firebase/database';

const firebaseConfig = {
  apiKey: "AIzaSyCwWU_TGjwe8nJiz5ZH4ktNnT-r_eFzi1k",
  authDomain: "bangchuyen-a2516.firebaseapp.com",
  databaseURL: "https://bangchuyen-a2516-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "bangchuyen-a2516",
  storageBucket: "bangchuyen-a2516.firebasestorage.app",
  messagingSenderId: "1022088954135",
  appId: "1:1022088954135:web:6cdce9724c05e10996b752",
};

const app = initializeApp(firebaseConfig);
export const db = getDatabase(app);
